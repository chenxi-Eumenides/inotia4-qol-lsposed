#include "plugin.h"
#include "claims.h"
#include "inventory_logging.h"
#include "native_inventory_hook.h"
#include "inventory_slot.h"

#include <cstdint>

#include "../../contracts/extension_bag.h"
#include "../../contracts/inventory_action.h"
#include "../../contracts/inventory_refresh.h"
#include "../../contracts/inventory_slot.h"
#include "../../contracts/world_facts.h"
#include "../../contracts/item_facts.h"
#include "sell_transaction.h"

// D2c-2a：native_inventory_hook / inventory_hook_stage4 已迁入本插件目录（原
// feature/patch）。claims 表与 backup 交还入口同层，经 claims.h 直接引用；原为避免
// feature→feature 层依赖而设的跨层全局桥前置声明已删除。
//
// D2c-2b-1：经 `inventory_refresh` capability 暴露 raw-original refresh trampoline，
// 供 extension_bag / move_merge 等消费者现取；出售门禁改由 hooks/dispatch 布尔门承载
// （谓词注册方 special_equip），本插件不再导出 set_sell_gate。
//
// B2a-1 禁止 deactivate：extension_bag / 自动出售 / 合成等 legacy 消费者直接依赖这 24 个
// hook 的 wrapper 语义（扩展袋读取、扩展补扣、出售门禁等）。deactivate 会由宿主物理
// 释放全部资源，令 legacy 消费者在 wrapper 缺失下运行；本阶段只允许进程内常驻。
// 清除条件：消费者改经 capability requires 收口（B2c/D2）后解除，见
// docs/refactor/map.md 过渡依赖区。宿主生命周期端点没有"不可 deactivate"标志，本约束
// 靠流程与文档保证，不在代码内拦截。

namespace qol::features::inventory_hooks {
namespace {

constexpr const char* kPluginId = "inventory_hooks";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_action_sdk = nullptr;
const contracts::QolGameHost* g_action_game = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
    {contracts::kItemFactsCapabilityId,
     {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}},
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    // extension_bag 的 optional_requires 包含 inventory_refresh；两条软边成环，
    // 宿主可降级其中一条软边破环。
    {contracts::kExtensionBagCapabilityId,
     {static_cast<uint16_t>(contracts::kExtensionBagVersionMajor),
      contracts::kExtensionBagSaleRemoveVersionMinor}},
};

// 能力接口实例：函数地址编译期常量；宿主在 enabled 时注册、revoking 时注销。
constexpr contracts::InventoryRefreshApi kInventoryRefreshApi{
    &inventory_native_hook_call_refresh_item_area_original,
};

struct SellTransactionContext {
    const contracts::QolGameHost* game = nullptr;
    const contracts::ExtensionBagApi* extension_prefix = nullptr;
    const contracts::ExtensionBagApi* extension_sale = nullptr;
    contracts::QolItemHandle item{};
    int32_t bag = -1;
    int32_t slot = -1;
    bool extension_item = false;
    int32_t refund_code = kQolOk;
};

QolResult sell_credit(void* raw, int64_t price) {
    auto* context = static_cast<SellTransactionContext*>(raw);
    if (context == nullptr || context->game == nullptr ||
        context->game->world_action.add_money == nullptr)
        return contracts::error(kQolErrNotReady, "sell credit unavailable", 23);
    return context->game->world_action.add_money(price);
}

QolResult sell_remove(void* raw) {
    auto* context = static_cast<SellTransactionContext*>(raw);
    if (context == nullptr || context->game == nullptr)
        return contracts::error(kQolErrInvalidArgument, "sell context unavailable", 24);
    if (context->extension_item) {
        if (context->extension_sale == nullptr ||
            context->extension_sale->remove_for_sale == nullptr)
            return contracts::error(kQolErrUnsupported, "extension sale unavailable", 26);
        return context->extension_sale->remove_for_sale(context->bag, context->slot,
                                                        context->item);
    }
    if (context->game->inventory_write.remove_item == nullptr)
        return contracts::error(kQolErrNotReady, "physical removal unavailable", 27);
    return context->game->inventory_write.remove_item(context->item);
}

QolResult sell_refund(void* raw, int64_t price) {
    auto* context = static_cast<SellTransactionContext*>(raw);
    if (context == nullptr || context->game == nullptr ||
        context->game->world_action.minus_money == nullptr)
        return contracts::error(kQolErrNotReady, "sell refund unavailable", 22);
    const QolResult result = context->game->world_action.minus_money(price);
    context->refund_code = result.code;
    return result;
}

contracts::InventorySellResult inventory_action_sell(int32_t bag, int32_t slot,
                                                     contracts::QolItemHandle expected_item,
                                                     const char* source) {
    contracts::InventorySellResult result{};
    const auto* facts = g_world_facts;
    if (facts == nullptr || facts->state == nullptr || g_action_game == nullptr ||
        g_item_facts == nullptr || g_item_facts->item_ref_at == nullptr) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    int32_t world_state = 0;
    if (!contracts::succeeded(facts->state(&world_state)) ||
        world_state != 5) {
        result.status = contracts::InventorySellStatus::kNotInWorld;
        return result;
    }
    if (bag == 5) {
        result.status = contracts::InventorySellStatus::kTaskBagExcluded;
        return result;
    }
    contracts::QolInventoryItemRef item_ref{};
    if (!contracts::succeeded(g_item_facts->item_ref_at(bag, slot, &item_ref))) {
        result.status = contracts::InventorySellStatus::kEmptySlot;
        return result;
    }
    if (item_ref.item.slot == 0 || item_ref.item.generation == 0) {
        result.status = contracts::InventorySellStatus::kNoItemHandle;
        return result;
    }
    if (expected_item.slot != item_ref.item.slot ||
        expected_item.generation != item_ref.item.generation) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    result.category = item_ref.category;
    result.count = item_ref.count;
    const auto kind = static_cast<contracts::QolInventoryItemKind>(item_ref.kind);
    if (kind != contracts::QolInventoryItemKind::kOriginal &&
        kind != contracts::QolInventoryItemKind::kProvider) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    const contracts::ExtensionBagApi* eb_prefix = nullptr;
    if (g_action_sdk != nullptr && g_action_sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        const QolResult required = g_action_sdk->capabilities.require(
            contracts::kExtensionBagCapabilityId,
            {static_cast<uint16_t>(contracts::kExtensionBagVersionMajor),
             static_cast<uint16_t>(contracts::kExtensionBagVersionMinor)},
            &iface);
        if (contracts::succeeded(required) && iface != nullptr)
            eb_prefix = static_cast<const contracts::ExtensionBagApi*>(iface);
    }
    const bool logical_bag = eb_prefix != nullptr && eb_prefix->is_logical_bag != nullptr &&
                             eb_prefix->is_logical_bag(bag);
    const bool extension_item = kind == contracts::QolInventoryItemKind::kProvider;
    if (extension_item != logical_bag) {
        result.status = extension_item ? contracts::InventorySellStatus::kExtensionSaleUnavailable
                                       : contracts::InventorySellStatus::kStale;
        return result;
    }
    const contracts::ExtensionBagApi* eb_sale = nullptr;
    if (extension_item) {
        int32_t identified_bag = -1;
        int32_t identified_slot = -1;
        if (eb_prefix == nullptr || eb_prefix->internal_index == nullptr ||
            eb_prefix->identify_native_item == nullptr) {
            result.status = contracts::InventorySellStatus::kStale;
            return result;
        }
        const int32_t internal_bag = eb_prefix->internal_index(bag);
        if (internal_bag < 0 ||
            !eb_prefix->identify_native_item(item_ref.item, &identified_bag, &identified_slot) ||
            identified_bag != internal_bag || identified_slot != slot) {
            result.status = contracts::InventorySellStatus::kStale;
            return result;
        }
        if (g_action_sdk == nullptr || g_action_sdk->capabilities.require == nullptr) {
            result.status = contracts::InventorySellStatus::kExtensionSaleUnavailable;
            return result;
        }
        // This separate negotiation is the only point at which the v3.1 tail is read.
        const void* sale_iface = nullptr;
        const QolResult negotiated = g_action_sdk->capabilities.require(
            contracts::kExtensionBagCapabilityId,
            {static_cast<uint16_t>(contracts::kExtensionBagVersionMajor),
             contracts::kExtensionBagSaleRemoveVersionMinor},
            &sale_iface);
        if (!contracts::succeeded(negotiated) || sale_iface == nullptr) {
            result.status = contracts::InventorySellStatus::kExtensionSaleUnavailable;
            return result;
        }
        eb_sale = static_cast<const contracts::ExtensionBagApi*>(sale_iface);
        if (eb_sale->remove_for_sale == nullptr) {
            result.status = contracts::InventorySellStatus::kExtensionSaleUnavailable;
            return result;
        }
    } else if (bag < 0 || bag > 4) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    if (g_item_facts == nullptr || g_item_facts->is_no_sell == nullptr) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    int32_t no_sell = 0;
    if (!contracts::succeeded(
            g_item_facts->is_no_sell(result.category, &no_sell))) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    if (no_sell != 0) {
        result.status = contracts::InventorySellStatus::kNoSell;
        return result;
    }
    if (!contracts::item_facts_api_quote_size_valid(g_item_facts) ||
        g_item_facts->quote == nullptr) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    contracts::QolSaleQuote quote{};
    quote.struct_size = sizeof(quote);
    if (!contracts::succeeded(
            g_item_facts->quote(item_ref.item, contracts::kDetailSell, &quote)) ||
        quote.struct_size < sizeof(quote) || quote.count <= 0 || quote.price < 0 ||
        quote.multiplier_den == 0 || quote.multiplier_num > quote.multiplier_den) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    const int32_t count = quote.count;
    const int64_t price = quote.price;
    if (price < 0 || g_action_game->world_action.add_money == nullptr ||
        g_action_game->world_action.minus_money == nullptr ||
        (!extension_item && g_action_game->inventory_write.remove_item == nullptr)) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    result.price = price;
    result.count = count;
    SellTransactionContext transaction{g_action_game, eb_prefix, eb_sale, item_ref.item,
                                        bag, slot, extension_item, kQolOk};
    result.status = run_sale_transaction(
        price, SaleTransactionOps{&transaction, &sell_credit, &sell_remove, &sell_refund});
    if (result.status == contracts::InventorySellStatus::kRollbackFailed) {
        QOL_LOG_ERROR(QolDomain::kInventory,
                       "inventory sell rollback failed source=%s bag=%d slot=%d price=%lld remove=%s refund_code=%d",
                      source != nullptr ? source : "unknown", bag, slot,
                      static_cast<long long>(price), extension_item ? "extension" : "original",
                      transaction.refund_code);
    } else if (result.status == contracts::InventorySellStatus::kRemoveFailed) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "inventory sell removal failed; refund succeeded source=%s bag=%d slot=%d",
                     source != nullptr ? source : "unknown", bag, slot);
    }
    if (result.status == contracts::InventorySellStatus::kOk && !extension_item &&
        eb_prefix != nullptr && eb_prefix->sync_projected_slot != nullptr)
        (void)eb_prefix->sync_projected_slot(bag, slot);
    return result;
}

constexpr contracts::InventoryActionApi kInventoryActionApi{
    &inventory_action_sell,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {
        contracts::kInventoryRefreshCapabilityId,
        {contracts::kInventoryRefreshVersionMajor, contracts::kInventoryRefreshVersionMinor},
        &kInventoryRefreshApi,
    },
    {
        contracts::kInventoryActionCapabilityId,
        {contracts::kInventoryActionVersionMajor, contracts::kInventoryActionVersionMinor},
        &kInventoryActionApi,
    },
    {
        contracts::kInventorySlotCapabilityId,
        {contracts::kInventorySlotVersion.major, contracts::kInventorySlotVersion.minor},
        &kInventorySlotApi,
    },
};

QolResult on_inventory_support_toggle(contracts::PluginContext&, uint8_t enabled) {
    inventory_native_hook_set_support(enabled != 0);
    return contracts::ok();
}

const contracts::FeatureDefinition kFeatures[] = {
    {
        "inventory_hooks.inventory_support",
        1,
        contracts::FeatureCategory::kHidden,
        contracts::FeatureType::kToggle,
        "扩展背包库存支持",
        "调试开关：关闭时库存 hook 全部透传原版；合并进 core.item 后删除。",
        &on_inventory_support_toggle,
        nullptr,
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.game == nullptr) {
        (void)inventory_native_hook_bind_sdk(nullptr);
        (void)inventory_native_hook_bind_game(nullptr);
        (void)inventory_native_hook_bind_item_facts(nullptr);
        return 1;
    }
    if (!inventory_native_hook_bind_sdk(ctx.sdk) || !inventory_native_hook_bind_game(ctx.game)) {
        return 1;
    }
    const void* item_facts_iface = nullptr;
    if (ctx.sdk->capabilities.require == nullptr ||
        !contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor},
            &item_facts_iface)) ||
        item_facts_iface == nullptr ||
        !contracts::item_facts_api_size_valid(
            static_cast<const contracts::ItemFactsApi*>(item_facts_iface)) ||
        !inventory_native_hook_bind_item_facts(
            static_cast<const contracts::ItemFactsApi*>(item_facts_iface))) {
        (void)inventory_native_hook_bind_item_facts(nullptr);
        return 1;
    }
    g_item_facts = static_cast<const contracts::ItemFactsApi*>(item_facts_iface);
    inventory_slot_bind(ctx.sdk, ctx.game, g_item_facts);
    const void* world_facts_iface = nullptr;
    if (ctx.sdk->capabilities.require == nullptr ||
        !contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor},
            &world_facts_iface)) ||
        world_facts_iface == nullptr) {
        return 1;
    }
    g_world_facts = static_cast<const contracts::WorldFactsApi*>(world_facts_iface);
    g_action_sdk = ctx.sdk;
    g_action_game = ctx.game;
    inventory_native_hook_set_support(true);
    // 自身初始化无额外动作：资源由宿主声明式安装，wrapper 业务体在本插件 TU。
    return 0;
}

// 资源物理释放由宿主逆序执行；门禁在 deactivate 后恢复默认启用状态。
void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_action_sdk = nullptr;
    g_action_game = nullptr;
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    inventory_slot_bind(nullptr, nullptr, nullptr);
    (void)inventory_native_hook_bind_item_facts(nullptr);
    inventory_native_hook_set_support(true);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    g_action_sdk = ctx.sdk;
    g_action_game = ctx.game;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

// 常驻无门禁：状态恒为 enabled。
void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;
    out.error = nullptr;
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    // 原函数入口交还 wrapper 所有者；同目录 claims.h 入口对未知 id / 空 backup 返回非 0
    // → 宿主逆序回滚本插件全部资源并置 failed（fail-closed，禁止 wrapper 在 backup 缺失下运行）。
    return inventory_hooks_bind_backup(resource_id, backup);
}

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    // claims 表本体在 native_inventory_hook.cpp（handler 取址限制），本插件同层直接引用；
    // 函数内静态保证描述符生命周期 ≥ 进程。
    static const contracts::PluginDescriptor descriptor = [] {
        uint32_t count = 0;
        const contracts::ResourceClaim* claims = inventory_hooks_claims(&count);
        contracts::PluginDescriptor descriptor{
            kPluginId,
            kPluginVersion,
            2, kRequires,  // requires=world.facts + item.facts
            1, kOptionalRequires,  // optional extension_bag v3.1 sale-removal operation
             3, kProvides,  // provides（inventory_refresh + inventory.action + inventory.slot）
            count, claims,
            0, nullptr,   // jni
            &plugin_install,
            &plugin_deactivate,
            &plugin_on_enabled,
            &plugin_on_disabled,
            &plugin_fill_status,
            &on_resource_ready,
            nullptr, 0, nullptr, 0,
             kFeatures, 1,
        };
        // Legacy consumers (extension_bag/autosell/craft) still call the 24 wrappers directly;
        // the host must reject deactivate until they migrate to capabilities.
        descriptor.deactivate_allowed = false;
        return descriptor;
    }();
    return descriptor;
}

const contracts::WorldFactsApi* world_facts() {
    return g_world_facts;
}

}  // namespace qol::features::inventory_hooks
