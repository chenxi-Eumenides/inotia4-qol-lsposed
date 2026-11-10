#include "plugin.h"

#include <cstdint>
#include <cstring>

#include "feature.h"
#include "../../contracts/character_facts.h"
#include "../../contracts/events.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/game_ports/character_extras_port.h"

// D2c-2b-1：出售按钮 UIEquip_ButtonDestroyExe 已被 inventory_hooks 单点 hook（同址
// NativeHook 二次挂载 rc=-1），本插件经 hooks/dispatch 布尔门登记谓词，不再前置声明
// inventory_hooks 的 set_sell_gate（跨插件符号依赖已切断）。resource_id 与 claim 一致。

namespace qol::features::special_equip {
namespace {

constexpr const char* kPluginId = "special_equip";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kSellGateResourceId = "hook:UIEquip_ButtonDestroyExe";

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kItemFactsCapabilityId,
     {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}},
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kCharacterFactsCapabilityId,
     {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor}},
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolCharacterExtrasPort* g_character_extras = nullptr;
// character.facts 是可选诊断能力：能力变更时据此重新缓存或清空。
uint64_t g_capability_event_subscription = 0;

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
     {contracts::kQolPortCharacterExtrasVersion.major,
      contracts::kQolPortCharacterExtrasVersion.minor}},
};

// 能力注册表变更：只关心 character.facts。注册后重新 require，撤销后清空缓存。
void on_capability_changed(void*, const void* payload) {
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::CapabilityChangedEvent*>(payload);
    if (event->id == nullptr ||
        std::strcmp(event->id, contracts::kCharacterFactsCapabilityId) != 0) {
        return;
    }
    if (event->available == 0) {
        set_character_facts(nullptr);
        return;
    }
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) {
        set_character_facts(nullptr);
        return;
    }
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kCharacterFactsCapabilityId,
            {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor},
            &iface)) ||
        iface == nullptr) {
        set_character_facts(nullptr);
        return;
    }
    set_character_facts(static_cast<const contracts::CharacterFactsApi*>(iface));
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

void clear_install_globals() {
    unsubscribe_capability_changes();
    set_gate(false);
    set_item_facts(nullptr);
    set_character_facts(nullptr);
    g_character_extras = nullptr;
    g_sdk = nullptr;
    g_game = nullptr;
}

// 资源声明（ARCHITECTURE.md §4.3 字段顺序）：两个入口 hook 均为私有、必需、fail-closed。
// 功能默认开启并作为 Hidden toggle 暴露：Debug 可关闭并触发生命周期卸载，Release 不显示 Hidden。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdCanEquipItem,     // id
        "F_CAN_EQUIP_VMA",          // symbol
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                        // resumable
        kPluginId,                   // owner
        "CHAR_CanEquipItem 是 +7 bit4 的唯一装备判定入口，需在入口跳过该位后重放其余检查；"
        "无函数指针槽可挂",          // reason
        nullptr,                     // gate（常驻）
        120,                         // step_budget
        reinterpret_cast<void*>(&can_equip_wrapper),  // handler
    },
    {
        kResourceIdCanUnequipItem,
        "F_CAN_UNEQUIP_VMA",        // symbol
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "CHAR_CanUnequipItem 是 +7 bit4 的唯一脱下判定入口，同上",
        nullptr,
        120,
        reinterpret_cast<void*>(&can_unequip_wrapper),
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    // 重新 install 可能未经过 deactivate；先撤销旧订阅再覆盖 sdk，避免订阅泄漏。
    unsubscribe_capability_changes();
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_character_extras = nullptr;
    set_item_facts(nullptr);
    set_character_facts(nullptr);
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr ||
        ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        clear_install_globals();
        return 1;
    }
    g_character_extras = static_cast<const contracts::QolCharacterExtrasPort*>(ctx.ports[0]);
    if (g_sdk->capabilities.require == nullptr) {
        clear_install_globals();
        return 1;
    }
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface)) ||
        iface == nullptr || !contracts::item_facts_api_size_valid(
                                static_cast<const contracts::ItemFactsApi*>(iface))) {
        clear_install_globals();
        return 1;
    }
    set_item_facts(static_cast<const contracts::ItemFactsApi*>(iface));
    // 先订阅再探测可选能力：避免订阅落地前 character.facts 恰好注册完成而错过通知。
    subscribe_capability_changes();
    iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kCharacterFactsCapabilityId,
            {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor},
            &iface)) ||
        iface == nullptr) {
        log_warn("optional dependency unavailable: %s", contracts::kCharacterFactsCapabilityId);
    } else {
        set_character_facts(static_cast<const contracts::CharacterFactsApi*>(iface));
    }
    // 默认开启，Hidden toggle 状态由生命周期回调驱动；Debug 可关闭并触发生命周期卸载，Release 不显示 Hidden。
    // 依赖缺失即 fail-closed 返回非 0，宿主不安装资源并置 failed。重复调用幂等（activate 重入）。
    if (!deps_ready()) {
        log_deps_missing();
        clear_install_globals();
        return 1;
    }
    set_gate(true);
    // 出售保护是本插件功能不变量，门禁登记失败不能静默降级。
    if (g_sdk->hooks.set_intercept == nullptr ||
        g_sdk->hooks.set_intercept(kSellGateResourceId, &sell_gate_should_block, nullptr).ok !=
            kQolOk) {
        log_error("sell gate unavailable: intercept register rejected id=%s", kSellGateResourceId);
        clear_install_globals();
        return 1;
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    unsubscribe_capability_changes();
    // 门禁置 false 并注销布尔门（幂等）；重复调用安全。
    set_gate(false);
    set_item_facts(nullptr);
    set_character_facts(nullptr);
    g_character_extras = nullptr;
    if (g_sdk != nullptr && g_sdk->hooks.clear_intercept != nullptr) {
        (void)g_sdk->hooks.clear_intercept(kSellGateResourceId);
    }
}

// 启停仅切门禁，不做资源操作（§3.3）。
void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = gate_enabled();
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"special_equip.equipment_rules", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "NPC装备", "允许获得、使用NPC装备",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    1, kRequires,
    1, kOptionalRequires,
    0, nullptr,   // provides
    2, kResources,
    0, nullptr,   // jni（无 JNI 绑定）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    kPorts,
    1,
    nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolGameHost* game_host() { return g_game; }

const contracts::QolCharacterExtrasPort* character_extras() { return g_character_extras; }

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::special_equip
