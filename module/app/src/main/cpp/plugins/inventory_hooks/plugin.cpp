#include "plugin.h"
#include "claims.h"
#include "native_inventory_hook.h"

#include <cstdint>

#include "../../contracts/extension_bag.h"
#include "../../contracts/inventory_action.h"
#include "../../contracts/inventory_refresh.h"
#include "../../contracts/sell_price.h"
#include "../../contracts/world_facts.h"

// D2c-2a：native_inventory_hook / inventory_hook_stage4 已迁入本插件目录（原
// feature/patch）。claims 表与 backup 交还入口同层，经 claims.h 直接引用；原为避免
// feature→feature 层依赖而设的跨层全局桥前置声明已删除。
//
// D2c-2b-1：经 `inventory_refresh` capability 暴露 raw-original refresh trampoline，
// 供 extension_bag / game_patch 等消费者现取；出售门禁改由 hooks/dispatch 布尔门承载
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

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
};

// 能力接口实例：函数地址编译期常量；宿主在 enabled 时注册、revoking 时注销。
constexpr contracts::InventoryRefreshApi kInventoryRefreshApi{
    &inventory_native_hook_call_refresh_item_area_original,
};

contracts::InventorySellResult inventory_action_sell(int32_t bag, int32_t slot,
                                                     const void* expected_item,
                                                     const char* source) {
    contracts::InventorySellResult result{};
    const auto* facts = g_world_facts;
    if (facts == nullptr || facts->state == nullptr || g_action_game == nullptr ||
        g_action_game->read_only.inventory.item_ref_at == nullptr) {
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
    if (!contracts::succeeded(g_action_game->read_only.inventory.item_ref_at(bag, slot, &item_ref))) {
        result.status = contracts::InventorySellStatus::kEmptySlot;
        return result;
    }
    if (item_ref.native_item == nullptr) {
        result.status = contracts::InventorySellStatus::kNoNativeItem;
        return result;
    }
    if (expected_item != nullptr && expected_item != item_ref.native_item) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    result.category = item_ref.category;
    result.count = item_ref.count;
    if (g_action_game->read_only.item.is_no_sell == nullptr ||
        g_action_game->read_only.item.cumulate_count == nullptr ||
        g_action_game->read_only.item.sell_price == nullptr) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    int32_t no_sell = 0;
    if (!contracts::succeeded(
            g_action_game->read_only.item.is_no_sell(result.category, &no_sell))) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    if (no_sell != 0) {
        result.status = contracts::InventorySellStatus::kNoSell;
        return result;
    }
    int32_t count = 0;
    if (!contracts::succeeded(
            g_action_game->read_only.item.cumulate_count(item_ref.native_item, &count)) ||
        count <= 0) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    int64_t unit_price = 0;
    if (!contracts::succeeded(
            g_action_game->read_only.item.sell_price(item_ref.native_item, &unit_price))) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    int64_t price = 0;
    const contracts::ExtensionBagApi* eb = nullptr;
    if (g_action_sdk != nullptr && g_action_sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        const QolResult capability_result = g_action_sdk->capabilities.require(
            contracts::kExtensionBagCapabilityId,
            QolVersionRange{static_cast<uint16_t>(contracts::kExtensionBagVersionMajor),
                            static_cast<uint16_t>(contracts::kExtensionBagVersionMinor)},
            &iface);
        if (contracts::succeeded(capability_result)) {
            eb = static_cast<const contracts::ExtensionBagApi*>(iface);
        }
    }
    if (eb != nullptr && eb->sell_price != nullptr) {
        price = eb->sell_price(item_ref.native_item, count, true);
    } else if (!contracts::sell_price::calculate(unit_price, static_cast<uint32_t>(count), true,
                                                 &price)) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    if (price < 0 || g_action_game->world_action.add_money == nullptr ||
        g_action_game->world_action.minus_money == nullptr ||
        g_action_game->inventory_write.remove_item == nullptr) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    result.price = price;
    result.count = count;
    if (!contracts::succeeded(g_action_game->world_action.add_money(price))) {
        result.status = contracts::InventorySellStatus::kCreditFailed;
        return result;
    }
    if (!contracts::succeeded(
            g_action_game->inventory_write.remove_item(item_ref.native_item))) {
        (void)g_action_game->world_action.minus_money(price);
        result.status = contracts::InventorySellStatus::kRemoveFailed;
        return result;
    }
    if (eb != nullptr && eb->sync_projected_slot != nullptr) eb->sync_projected_slot(bag, slot);
    (void)source;
    result.status = contracts::InventorySellStatus::kOk;
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
};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.game == nullptr) {
        (void)inventory_native_hook_bind_sdk(nullptr);
        (void)inventory_native_hook_bind_game(nullptr);
        return 1;
    }
    if (!inventory_native_hook_bind_sdk(ctx.sdk) || !inventory_native_hook_bind_game(ctx.game)) {
        return 1;
    }
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
    // 自身初始化无额外动作：资源由宿主声明式安装，wrapper 业务体在本插件 TU。
    return 0;
}

// 空操作幂等：资源物理释放由宿主逆序执行，本插件无额外状态需要清理。
void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_action_sdk = nullptr;
    g_action_game = nullptr;
    g_world_facts = nullptr;
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
        return contracts::PluginDescriptor{
            kPluginId,
            kPluginVersion,
            1, kRequires,  // requires=world.facts
            0, nullptr,   // optional_requires
            2, kProvides,  // provides（inventory_refresh + inventory.action v1.0）
            count, claims,
            0, nullptr,   // jni
            &plugin_install,
            &plugin_deactivate,
            &plugin_on_enabled,
            &plugin_on_disabled,
            &plugin_fill_status,
            &on_resource_ready,
        };
    }();
    return descriptor;
}

const contracts::WorldFactsApi* world_facts() {
    return g_world_facts;
}

}  // namespace qol::features::inventory_hooks
