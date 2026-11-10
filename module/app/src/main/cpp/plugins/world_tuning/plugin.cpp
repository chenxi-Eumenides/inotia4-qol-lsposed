#include "plugin.h"

#include <cstdint>
#include <cstdio>

#include "../../contracts/character_facts.h"
#include "../../contracts/game_ports/character_extras_port.h"
#include "../../contracts/item_use.h"
#include "../../contracts/panels.h"
#include "../../contracts/ui.h"
#include "feature.h"
#include "panel.h"
#include "rates.h"

namespace qol::features::world_tuning {
namespace {

constexpr const char* kPluginId = "world_tuning";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolCharacterExtrasPort* g_character_extras = nullptr;
const contracts::CharacterFactsApi* g_character_facts = nullptr;
const char* g_install_error = nullptr;
bool g_panel_registered = false;
uint64_t g_item_use_task = 0;

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kCharacterFactsCapabilityId,
     {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor}},
};

// 面板是辅助能力：界面工具被停用时倍率仍然生效，只影响面板可用性。
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kUiPanelsCapabilityId,
     {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
      static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)}},
    {contracts::kUiComponentsCapabilityId,
     {contracts::kUiContractMajor, contracts::kUiContractMinor}},
    // item.use 由 core.item 提供；缺失时只影响药水冷却，其他倍率照常工作。
    {contracts::kItemUseCapabilityId,
     {contracts::kItemUseVersion.major, contracts::kItemUseVersion.minor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
     {contracts::kQolPortCharacterExtrasVersion.major,
      contracts::kQolPortCharacterExtrasVersion.minor}},
};

// 两个入口 hook 均为 optional + fail-soft：某个落点不可用时只影响对应倍率，
// 插件仍保持可用（面板可打开）；资源安装失败不把插件置 failed。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdAddDamage,
        "F_CHAR_ADD_DAMAGE_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "无函数指针槽，15 个静态调用点经此单点汇合；monster 变体入口被外部周期性还原，跟随入口 b 的隐藏段目标",
        nullptr,
        120,
        reinterpret_cast<void*>(&char_add_damage_wrapper),
        false,
        true,
    },
    {
        kResourceIdUpdateAttrFromMonster,
        "F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "最大生命重算唯一入口",
        nullptr,
        120,
        reinterpret_cast<void*>(&char_update_attr_from_monster_wrapper),
    },
    {
        kResourceIdAddExperience,
        "F_ADD_EXP_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "加经验统一入口；按接收者阵营缩放",
        nullptr,
        120,
        reinterpret_cast<void*>(&char_add_experience_wrapper),
    },
    {
        kResourceIdAddMoney,
        "F_ADD_MONEY_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "加金币统一增量入口；按倍率缩放入参",
        nullptr,
        120,
        reinterpret_cast<void*>(&inven_add_money_wrapper),
    },
    {
        kResourceIdDropItem,
        "F_CHARSYSTEM_DROP_ITEM_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "死亡掉落整次结算入口；数量倍率重复调用，重抽在上下文内生效",
        nullptr,
        120,
        reinterpret_cast<void*>(&drop_item_wrapper),
    },
    {
        kResourceIdMakeItem,
        "F_MAKE_ITEM_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "掉落槽内造物（卷轴/宝石/背包/骰子）；重抽次数按同类别重掷取最优",
        nullptr,
        120,
        reinterpret_cast<void*>(&make_item_wrapper),
    },
    {
        kResourceIdMakeEquipDetail,
        "F_MAKE_EQUIP_DETAIL_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "装备候选选取；kind/等级段固定，重抽次数只重掷候选稀有度",
        nullptr,
        120,
        reinterpret_cast<void*>(&make_equip_detail_wrapper),
    },
    {
        kResourceIdGetActionCoolTime,
        "F_CHAR_GET_ACTION_COOLTIME_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "技能冷却时长唯一入口；按玩家技能冷却倍率缩放返回值",
        nullptr,
        120,
        reinterpret_cast<void*>(&char_get_action_cooltime_wrapper),
    },
    {
        kResourceIdSetActionNextTime,
        "F_CHAR_SET_ACTION_NEXT_TIME_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "动作冷却结束时间写入；按最终 delta 兜底缩放，覆盖 monster 变体在原始计算后直接覆盖冷却值的特殊逻辑",
        nullptr,
        120,
        reinterpret_cast<void*>(&char_set_action_next_time_wrapper),
        false,
        true,
    },
};

void clear_install_state() {
    panel_unbind();
    clear_runtime();
    g_sdk = nullptr;
    g_game = nullptr;
    g_character_extras = nullptr;
    g_character_facts = nullptr;
}

// 面板为可选能力：install 阶段 UI 能力可能仍在 resolving，enable 与 entry 时各重试一次。
// 只有 ui_panels + ui_components 同时可用才注册 provider；失败不影响倍率接线。
void ensure_panel(contracts::PluginContext& ctx) {
    if (g_panel_registered || ctx.sdk == nullptr || ctx.sdk->capabilities.require == nullptr ||
        ctx.sdk->panels.register_provider == nullptr || ctx.sdk->ui == nullptr) {
        return;
    }
    const void* panels_iface = nullptr;
    const void* components_iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kUiPanelsCapabilityId,
            {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
             static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)},
            &panels_iface)) ||
        panels_iface == nullptr ||
        !contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kUiComponentsCapabilityId,
            {contracts::kUiContractMajor, contracts::kUiContractMinor},
            &components_iface)) ||
        components_iface == nullptr) {
        return;
    }
    if (!contracts::succeeded(ctx.sdk->panels.register_provider(&panel_provider()))) return;
    panel_bind(ctx.sdk, static_cast<const contracts::QolUiComponents*>(components_iface),
               ctx.sdk->ui);
    g_panel_registered = true;
}

void ensure_item_use() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return;
    const void* iface = nullptr;
    if (contracts::succeeded(g_sdk->capabilities.require(
            contracts::kItemUseCapabilityId,
            {contracts::kItemUseVersion.major, contracts::kItemUseVersion.minor},
            &iface)) &&
        iface != nullptr) {
        bind_item_use(static_cast<const contracts::ItemUseApi*>(iface));
    } else {
        unbind_item_use();
    }
}

int32_t item_use_bind_tick(int64_t, void*) {
    ensure_item_use();
    if (item_use_bound()) {
        g_item_use_task = 0;
        return 0;
    }
    return 1;
}

void start_item_use_retry() {
    if (g_item_use_task != 0 || item_use_bound()) return;
    if (g_sdk == nullptr || g_sdk->scheduler.add == nullptr) return;
    uint64_t task = 0;
    if (contracts::succeeded(
            g_sdk->scheduler.add(1, &item_use_bind_tick, nullptr, 30, 0, &task))) {
        g_item_use_task = task;
    }
}

void stop_item_use_retry() {
    if (g_item_use_task != 0 && g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
        (void)g_sdk->scheduler.remove(g_item_use_task);
    }
    g_item_use_task = 0;
}

int plugin_install(contracts::PluginContext& ctx) {
    set_gate(false);
    g_sdk = nullptr;
    g_game = nullptr;
    g_character_extras = nullptr;
    g_character_facts = nullptr;
    g_install_error = nullptr;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.game == nullptr ||
        ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        clear_install_state();
        g_install_error = "install failed: context invalid";
        return 1;
    }
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_character_extras = static_cast<const contracts::QolCharacterExtrasPort*>(ctx.ports[0]);
    if (g_sdk->capabilities.require == nullptr) {
        clear_install_state();
        g_install_error = "install failed: capabilities unavailable";
        return 1;
    }
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kCharacterFactsCapabilityId,
            {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor},
            &iface)) ||
        iface == nullptr) {
        clear_install_state();
        g_install_error = "install failed: character.facts unavailable";
        return 1;
    }
    g_character_facts = static_cast<const contracts::CharacterFactsApi*>(iface);
    bind_runtime(g_sdk, g_game, g_character_extras, g_character_facts, ctx.owner_token, kPluginId);

    ensure_panel(ctx);
    ensure_item_use();
    start_item_use_retry();
    reload_rates_from_config();
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    set_gate(false);
    stop_item_use_retry();
    unbind_item_use();
    if (ctx.sdk != nullptr && ctx.sdk->panels.unregister_owner != nullptr)
        (void)ctx.sdk->panels.unregister_owner(kPluginId);
    panel_unbind();
    g_panel_registered = false;
    // 保留 sdk/game：残留 wrapper 仍需要 in-flight 记账与原函数 backup。
    bind_runtime(g_sdk, g_game, nullptr, nullptr, {}, kPluginId);
    g_character_extras = nullptr;
    g_character_facts = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    set_gate(true);
    ensure_panel(ctx);
    ensure_item_use();
    start_item_use_retry();
    reload_rates_from_config();
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
    stop_item_use_retry();
    unbind_item_use();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = gate_enabled();
    out.error = g_install_error;
}

QolResult open_panel_feature(contracts::PluginContext& ctx) {
    if (!gate_enabled()) {
        return contracts::error(kQolErrNotReady, "world tuning is not enabled", 29);
    }
    ensure_panel(ctx);
    if (!panel_available()) {
        return contracts::error(kQolErrNotReady, "ui tools unavailable", 19);
    }
    if (ctx.sdk == nullptr || ctx.sdk->capabilities.require == nullptr) {
        return contracts::error(kQolErrNotReady, "ui_panels unavailable", 21);
    }
    const void* iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kUiPanelsCapabilityId,
            {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
             static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)},
            &iface)) ||
        iface == nullptr) {
        return contracts::error(kQolErrNotReady, "ui_panels unavailable", 21);
    }
    const auto* panels = static_cast<const contracts::UiPanelsApi*>(iface);
    if (panels->open == nullptr || !panels->open("world_tuning")) {
        return contracts::error(kQolErrBlocked, "world tuning panel could not be opened", 35);
    }
    return contracts::ok();
}

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"world_tuning.panel", 0, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kEntry, "游戏世界调整", "调整游戏世界的属性",
     nullptr, &open_panel_feature, "调整", 0, nullptr, 0, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    1, kRequires,          // requires=character.facts
    3, kOptionalRequires,  // optional=ui_panels + ui_components + item.use
    0, nullptr,            // provides
    9, kResources,
    0, nullptr,            // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    kPorts,
    1,
    nullptr, 0,  // deprecated http queries
    kFeatures, 1,
    nullptr, 0,                // http endpoints
    &validate_rates_config,    // config schema: rates only
    nullptr, 0,                // http route metadata
    true,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::world_tuning
