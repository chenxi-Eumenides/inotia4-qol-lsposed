#include "plugin.h"

#include <cstdint>
#include <cstring>

#include "../../contracts/attribute_quality.h"
#include "../../contracts/craft_commands.h"
#include "../../contracts/craft_facts.h"
#include "../../contracts/game_ports/craft_ui_write_port.h"
#include "../../contracts/game_ports/custom_recipe_write_port.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/text_layer.h"
#include "../../contracts/version.h"
#include "feature.h"
#include "table.h"
#include "text_table.h"
#include "ui/feature.h"
#include "ui/runtime.h"

// 自定义合成配方插件：结果层 + 表注入行为所有权 + UIMix 界面子层（单一 owner）。
//
// 门禁：descriptor feature `custom_recipe.recipes`（默认 false，用户可见唯一开关）。
//   · feature 开 → 插件活跃：required 依赖/端口校验 → 内部注册（文本消费者、typed handlers、
//     UI 本地备份）→ 资源事务（5 核心 required fail-closed + 7 装饰 optional fail-soft）→
//     ownership（RECIPEBASE/MIXTUREBASE 注入）→ on_enabled 在 ownership 就绪且核心 hook 备齐时
//     打开业务门禁。
//   · feature 关 → on_toggle 立即 set_enabled(false)（残留面板 fail-closed、在途确认作废）；
//     宿主 drain 后按资源逆序释放：ownership 先 release 恢复表记录，再释放 hooks。
// craft_result 不再经 capability registry 对外提供；UI 子层直接调用父命名空间结果 API。

namespace qol::features::custom_recipe {
namespace {

constexpr const char* kPluginId = "custom_recipe";
constexpr const char* kPluginVersion = "0.1.0";

// RECIPEBASE/MIXTUREBASE 注入所有权的资源 id：claim 声明与 on_resource_ready 识别共用同一常量。
constexpr const char* kOwnershipResourceId = "own:RECIPEBASE_MIXTUREBASE";

// ---- 「宝石强化」呈现文案（ADR 0001 D3：合成域的配方呈现文案归本插件）----
constexpr int32_t kTextPhaseAfter = 1;
constexpr int32_t kTextKindModify = 1;

const contracts::TextLayerApi* g_text_api = nullptr;
uint64_t g_text_consumer = 0;

// after 阶段消费者：仅当 UI 子层当前 scope 与 id 同时命中时返回模块文案。
const char* recipe_text_modify(uint32_t text_id, const char* current, void* ctx) {
    (void)current;
    (void)ctx;
    return text_lookup(ui::current_text_scope(), text_id);
}

bool write_stuff_need_handler(void*, const uint8_t* payload, uint32_t size);
bool set_stuff_item_handler(void*, const uint8_t* payload, uint32_t size);

// 资源声明：runtime 的 12 条 UI hook（5 required + 7 optional）在前，表 ownership 最后。
// 宿主停用时逆序释放 → ownership 先 release（恢复表记录），UI hooks 后释放。
const contracts::ResourceClaim& ownership_claim() {
    static const contracts::ResourceClaim kClaim = {
        kOwnershipResourceId,
        "RECIPEBASE_MIXTUREBASE",
        contracts::ResourceMechanism::kOwnership,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "RECIPEBASE/MIXTUREBASE 注入记录的占有：插件停用时收回记录数、残留面板不命中注入记录",
        nullptr,
        120,
        const_cast<contracts::OwnershipOps*>(&ownership_ops()),
    };
    return kClaim;
}

struct ResourceClaimTable {
    contracts::ResourceClaim items[16];
    uint32_t count = 0;
};

const ResourceClaimTable& resource_claim_table() {
    static const ResourceClaimTable table = [] {
        ResourceClaimTable combined{};
        uint32_t ui_count = 0;
        const contracts::ResourceClaim* ui_claims = ui::claims(&ui_count);
        for (uint32_t i = 0; i < ui_count && combined.count < 16; ++i) {
            combined.items[combined.count++] = ui_claims[i];
        }
        if (combined.count < 16) combined.items[combined.count++] = ownership_claim();
        return combined;
    }();
    return table;
}

bool ui_write_port_ready(const contracts::QolCraftUiWritePort* port) {
    if (port == nullptr ||
        !contracts::satisfies(port->abi_version,
                              {contracts::kQolPortCraftUiWriteVersion.major,
                               contracts::kQolPortCraftUiWriteVersion.minor})) {
        return false;
    }
    return port->init_mixing_state != nullptr && port->reset_stuff_item_control != nullptr &&
           port->refresh_inventory_items != nullptr &&
           port->select_first_empty_stuff_slot != nullptr && port->set_ui_type != nullptr &&
           port->set_selected_recipe != nullptr && port->play_sound != nullptr &&
           port->show_text != nullptr && port->show_yesno_from_text != nullptr &&
           port->set_target_slot_item != nullptr && port->set_cost_zero != nullptr;
}

bool recipe_write_port_ready(const contracts::QolCustomRecipeWritePort* port) {
    if (port == nullptr ||
        !contracts::satisfies(port->abi_version,
                              {contracts::kQolPortCustomRecipeWriteVersion.major,
                               contracts::kQolPortCustomRecipeWriteVersion.minor})) {
        return false;
    }
    return port->write_stuff_need != nullptr && port->set_stuff_item != nullptr &&
           port->description_set_off != nullptr &&
           (port->create_perfect_item != nullptr || port->create_item != nullptr) &&
           port->save_item != nullptr && port->free_item != nullptr &&
           port->read_jewel_word != nullptr && port->write_jewel_word != nullptr &&
           port->set_max_socket_enchant != nullptr && port->random_inclusive != nullptr;
}

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.game == nullptr || ctx.sdk->capabilities.require == nullptr) {
        return 1;
    }
    if (ctx.ports == nullptr || ctx.port_count != 2 || ctx.ports[0] == nullptr ||
        ctx.ports[1] == nullptr) {
        return 1;
    }
    const auto* recipe_write_port =
        static_cast<const contracts::QolCustomRecipeWritePort*>(ctx.ports[0]);
    const auto* ui_write_port =
        static_cast<const contracts::QolCraftUiWritePort*>(ctx.ports[1]);
    if (!recipe_write_port_ready(recipe_write_port) || !ui_write_port_ready(ui_write_port)) {
        return 1;
    }
    if (ctx.sdk->commands.register_typed_handler == nullptr ||
        ctx.sdk->commands.unregister_typed_handler == nullptr) {
        return 1;
    }

    const void* iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kTextLayerCapabilityId,
            {contracts::kTextLayerVersionMajor, contracts::kTextLayerVersionMinor}, &iface)) ||
        iface == nullptr) {
        return 1;
    }
    const auto* text_api = static_cast<const contracts::TextLayerApi*>(iface);
    if (text_api->subscribe == nullptr || text_api->unsubscribe == nullptr) return 1;

    iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface)) ||
        iface == nullptr ||
        !contracts::item_facts_api_size_valid(static_cast<const contracts::ItemFactsApi*>(iface))) {
        return 1;
    }
    const auto* item_facts = static_cast<const contracts::ItemFactsApi*>(iface);

    iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kCraftFactsCapabilityId,
            {contracts::kCraftFactsVersion.major, contracts::kCraftFactsVersion.minor}, &iface)) ||
        iface == nullptr) {
        return 1;
    }
    const auto* craft_facts = static_cast<const contracts::CraftFactsApi*>(iface);

    // 通过校验后开始登记；任何中途失败立即幂等清理本次注册，不依赖 Host 后帧清理。
    const uint64_t text_consumer =
        text_api->subscribe(kTextPhaseAfter, kTextKindModify, &recipe_text_modify, nullptr);
    if (text_consumer == 0) return 1;
    if (ctx.sdk->commands
            .register_typed_handler(contracts::kCraftWriteStuffNeedHandlerId,
                                    &write_stuff_need_handler, nullptr)
            .ok != kQolOk) {
        text_api->unsubscribe(text_consumer);
        return 1;
    }
    if (ctx.sdk->commands
            .register_typed_handler(contracts::kCraftSetStuffItemHandlerId,
                                    &set_stuff_item_handler, nullptr)
            .ok != kQolOk) {
        (void)ctx.sdk->commands.unregister_typed_handler(
            contracts::kCraftWriteStuffNeedHandlerId);
        text_api->unsubscribe(text_consumer);
        return 1;
    }
    if (!ui::bind(ctx, item_facts, craft_facts, recipe_write_port, ui_write_port, text_api)) {
        (void)ctx.sdk->commands.unregister_typed_handler(
            contracts::kCraftSetStuffItemHandlerId);
        (void)ctx.sdk->commands.unregister_typed_handler(
            contracts::kCraftWriteStuffNeedHandlerId);
        text_api->unsubscribe(text_consumer);
        return 1;
    }

    g_text_api = text_api;
    g_text_consumer = text_consumer;
    set_item_facts(item_facts);
    set_craft_facts(craft_facts);
    set_write_port(recipe_write_port);
    set_context(ctx);
    // 进档重建动态特殊装备配方；订阅经 Host SDK 事件总线完成。
    register_dynamic_recipes();
    // 门禁由 on_enabled 打开：ownership 就绪且核心 hook 备齐前不得执行业务。
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    set_enabled(false);
    if (g_text_api != nullptr && g_text_api->unsubscribe != nullptr && g_text_consumer != 0) {
        g_text_api->unsubscribe(g_text_consumer);
    }
    g_text_consumer = 0;
    g_text_api = nullptr;
    if (host_sdk() != nullptr && host_sdk()->commands.unregister_typed_handler != nullptr) {
        (void)host_sdk()->commands.unregister_typed_handler(
            contracts::kCraftWriteStuffNeedHandlerId);
        (void)host_sdk()->commands.unregister_typed_handler(contracts::kCraftSetStuffItemHandlerId);
    }
    unregister_dynamic_recipes();
    ui::clear();
    set_item_facts(nullptr);
    set_craft_facts(nullptr);
    set_write_port(nullptr);
    ::custom_recipe::custom_recipe_table_deactivate();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.game == nullptr) return;
    set_context(ctx);
    // 仅当表 ownership 就绪且 5 条核心 hook 备份齐备时才打开业务门禁。
    if (ownership_ready() && ui::core_backups_ready()) {
        set_enabled(true);
    }
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    // 先关业务门禁并作废在途确认；随后宿主 drain 并按资源逆序释放（ownership 先 release）。
    set_enabled(false);
    ::custom_recipe::custom_recipe_table_deactivate();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = enabled();
    out.error = nullptr;
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    // 定向 ownership 恢复：Host 仅在单 claim 恢复成功时以 backup=nullptr 回调本条。
    // 这里只基于 ownership_ready() 与 UI 核心 hook 备份就绪重算业务门禁：不注册资源、
    // 不读写游戏内存、不重复 on_enabled 的其它动作（其他插件未承诺 on_enabled 可重复调用）。
    if (resource_id != nullptr && std::strcmp(resource_id, kOwnershipResourceId) == 0) {
        set_context(ctx);
        if (ownership_ready() && ui::core_backups_ready()) {
            set_enabled(true);
        }
        return 0;
    }
    return ui::on_resource_ready(ctx, resource_id, backup);
}

// optional_requires=[attribute.quality]：可选依赖缺失/失效只置 degraded，不级联停用。
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kAttributeQualityCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kAttributeQualityVersionMajor),
                     static_cast<uint16_t>(contracts::kAttributeQualityVersionMinor)}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kItemFactsCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kItemFactsVersionMajor),
                      static_cast<uint16_t>(contracts::kItemFactsVersionMinor)}},
    {contracts::kCraftFactsCapabilityId,
     {contracts::kCraftFactsVersion.major, contracts::kCraftFactsVersion.minor}},
    // 配方呈现文案经文本层能力登记（ADR 0001 D3）。
    {contracts::kTextLayerCapabilityId,
     {static_cast<uint16_t>(contracts::kTextLayerVersionMajor),
      static_cast<uint16_t>(contracts::kTextLayerVersionMinor)}},
};

// 两个写端口：custom recipe 结果/材料写面 + UIMix 控件写面（合并前分属两个 descriptor）。
const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CUSTOM_RECIPE_WRITE,
     {contracts::kQolPortCustomRecipeWriteVersion.major,
      contracts::kQolPortCustomRecipeWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_CRAFT_UI_WRITE,
     {contracts::kQolPortCraftUiWriteVersion.major,
      contracts::kQolPortCraftUiWriteVersion.minor}},
};

bool write_stuff_need_handler(void*, const uint8_t* payload, uint32_t size) {
    return handle_write_stuff_need_command(payload, size);
}

bool set_stuff_item_handler(void*, const uint8_t* payload, uint32_t size) {
    return handle_set_stuff_item_command(payload, size);
}

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    const ResourceClaimTable& claim_table = resource_claim_table();
    // 唯一可见开关。on_toggle 关闭时同步镜像到内部 enabled（残留面板 fail-closed）；
    // 打开时不提前开门禁，由 on_enabled 在资源就绪后处理。
    static constexpr contracts::FeatureDefinition kFeatures[] = {
        {"custom_recipe.recipes", 0, contracts::FeatureCategory::kGameplay,
         contracts::FeatureType::kToggle, "自定义合成", "提供自定义配方与合成结果。",
         [](contracts::PluginContext&, uint8_t enabled) -> QolResult {
             if (enabled == 0) set_enabled(false);
             return contracts::ok();
          },
          nullptr, nullptr, -1},
    };
    static const contracts::PluginDescriptor descriptor = {
        kPluginId,
        kPluginVersion,
        3, kRequires,            // requires=item.facts + craft.facts + text_layer
        1, kOptionalRequires,    // optional_requires=[attribute.quality]
        0, nullptr,              // provides=无（craft_result 内部化）
        claim_table.count, claim_table.items,  // resources=12 UI hooks + ownership
        0, nullptr,              // jni
        &plugin_install,
        &plugin_deactivate,
        &plugin_on_enabled,
        &plugin_on_disabled,
        &plugin_fill_status,
        &on_resource_ready,
        kPorts, 2,
        nullptr, 0,
        kFeatures, 1,
    };
    return descriptor;
}

}  // namespace qol::features::custom_recipe
