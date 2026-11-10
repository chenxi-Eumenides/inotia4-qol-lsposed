#include "plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../contracts/character_facts.h"
#include "../../contracts/events.h"
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
// item.use 与面板 UI 能力均为可选：由宿主在能力注册/撤销时广播变更，
// 插件据此重新 require、注册 provider 或解绑/注销。
uint64_t g_capability_event_subscription = 0;

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
        "技能冷却时长唯一缩放源；c18 hook 正常入口，monster 跟随入口 b 到隐藏段函数体后 hook",
        nullptr,
        120,
        reinterpret_cast<void*>(&char_get_action_cooltime_wrapper),
        false,
        true,
        true,
    },
    {
        kResourceIdDropReroll258,
        "INSN_DROP_REROLL_258",
        contracts::ResourceMechanism::kInsnPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "在掉落阈值比较前应用重抽 K²；保留原 roll 与 CMP 条件标志",
        nullptr,
        120,
        reinterpret_cast<void*>(&drop_threshold_patch_helper),
    },
    {
        kResourceIdDropReroll3B8,
        "INSN_DROP_REROLL_3B8",
        contracts::ResourceMechanism::kInsnPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "在掉落阈值比较前应用重抽 K²；保留原 roll 与 CMP 条件标志",
        nullptr,
        120,
        reinterpret_cast<void*>(&drop_threshold_patch_helper),
    },
    {
        kResourceIdDropReroll400,
        "INSN_DROP_REROLL_400",
        contracts::ResourceMechanism::kInsnPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "在掉落阈值比较前应用重抽 K²；保留原 roll 与 CMP 条件标志",
        nullptr,
        120,
        reinterpret_cast<void*>(&drop_threshold_patch_helper),
    },
    {
        kResourceIdDropReroll460,
        "INSN_DROP_REROLL_460",
        contracts::ResourceMechanism::kInsnPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailSoft,
        true,
        kPluginId,
        "在掉落阈值比较前应用重抽 K²；保留原 roll 与 CMP 条件标志",
        nullptr,
        120,
        reinterpret_cast<void*>(&drop_threshold_patch_helper),
    },
};

// 面板 provider 注销与绑定缓存失效；deactivate 与能力撤销共用。
void unregister_panel() {
    if (g_sdk != nullptr && g_panel_registered && g_sdk->panels.unregister_owner != nullptr) {
        (void)g_sdk->panels.unregister_owner(kPluginId);
    }
    panel_unbind();
    g_panel_registered = false;
}

void clear_install_state() {
    // install 失败路径也要撤销事件订阅，避免订阅表残留指向已清空状态的处理器。
    if (g_sdk != nullptr && g_sdk->events.unsubscribe != nullptr &&
        g_capability_event_subscription != 0) {
        (void)g_sdk->events.unsubscribe(g_capability_event_subscription);
    }
    g_capability_event_subscription = 0;
    // 先于 g_sdk 清空调用：注销依赖当前 sdk 的 panels 接口。
    unregister_panel();
    clear_runtime();
    g_sdk = nullptr;
    g_game = nullptr;
    g_character_extras = nullptr;
    g_character_facts = nullptr;
}

// 面板为可选能力：install 阶段 UI 能力可能仍在 resolving，enable、entry 与能力变更时重试。
// 只有 ui_panels + ui_components 同时可用才注册 provider；失败不影响倍率接线。
void ensure_panel() {
    if (g_panel_registered || g_sdk == nullptr || g_sdk->capabilities.require == nullptr ||
        g_sdk->panels.register_provider == nullptr || g_sdk->ui == nullptr) {
        return;
    }
    const void* panels_iface = nullptr;
    const void* components_iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kUiPanelsCapabilityId,
            {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
             static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)},
            &panels_iface)) ||
        panels_iface == nullptr ||
        !contracts::succeeded(g_sdk->capabilities.require(
            contracts::kUiComponentsCapabilityId,
            {contracts::kUiContractMajor, contracts::kUiContractMinor},
            &components_iface)) ||
        components_iface == nullptr) {
        return;
    }
    if (!contracts::succeeded(g_sdk->panels.register_provider(&panel_provider()))) return;
    panel_bind(g_sdk, static_cast<const contracts::QolUiComponents*>(components_iface), g_sdk->ui);
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

// 能力注册表变更：item.use 重绑订阅；ui_panels/ui_components 注册时补注册面板，
// 任一撤销时注销 provider 并失效绑定缓存。available=1 只在插件启用时生效，
// 避免禁用期间的事件把已收回的 provider/订阅重新挂上；available=0 始终清理。
void on_capability_changed(void*, const void* payload) {
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::CapabilityChangedEvent*>(payload);
    if (event->id == nullptr) return;
    if (std::strcmp(event->id, contracts::kItemUseCapabilityId) == 0) {
        if (event->available == 0) {
            unbind_item_use();
        } else if (gate_enabled()) {
            ensure_item_use();
        }
        return;
    }
    if (std::strcmp(event->id, contracts::kUiPanelsCapabilityId) != 0 &&
        std::strcmp(event->id, contracts::kUiComponentsCapabilityId) != 0) {
        return;
    }
    if (event->available == 0) {
        unregister_panel();
    } else if (gate_enabled()) {
        ensure_panel();
    }
}

void subscribe_capability_changes() {
    if (g_sdk == nullptr || g_sdk->events.subscribe == nullptr) return;
    if (g_capability_event_subscription != 0) return;
    uint64_t subscription = 0;
    if (contracts::succeeded(g_sdk->events.subscribe(contracts::kEventCapabilityChanged,
                                                     &on_capability_changed, nullptr,
                                                     &subscription))) {
        g_capability_event_subscription = subscription;
    }
}

void unsubscribe_capability_changes() {
    if (g_capability_event_subscription == 0) return;
    if (g_sdk != nullptr && g_sdk->events.unsubscribe != nullptr) {
        (void)g_sdk->events.unsubscribe(g_capability_event_subscription);
    }
    g_capability_event_subscription = 0;
}

int plugin_install(contracts::PluginContext& ctx) {
    set_gate(false);
    // 重新 install 可能未经过 deactivate；先撤销旧订阅与面板注册再覆盖 sdk，避免泄漏。
    unsubscribe_capability_changes();
    unregister_panel();
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

    // 先订阅再探测：避免订阅落地前可选能力恰好注册完成而错过通知。
    subscribe_capability_changes();
    ensure_panel();
    ensure_item_use();
    reload_rates_from_config();
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
    unsubscribe_capability_changes();
    unbind_item_use();
    unregister_panel();
    // 保留 sdk/game：残留 wrapper 仍需要 in-flight 记账与原函数 backup。
    bind_runtime(g_sdk, g_game, nullptr, nullptr, {}, kPluginId);
    g_character_extras = nullptr;
    g_character_facts = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(true);
    ensure_panel();
    ensure_item_use();
    reload_rates_from_config();
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
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
    ensure_panel();
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
    12, kResources,
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
