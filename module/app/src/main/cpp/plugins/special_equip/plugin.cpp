#include "plugin.h"

#include <cstdint>

#include "feature.h"
#include "../../contracts/character_facts.h"
#include "../../contracts/item_facts.h"

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
    {contracts::kCharacterFactsCapabilityId,
     {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor}},
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;

// 资源声明（ARCHITECTURE.md §4.3 字段顺序）：两个入口 hook 均为私有、必需、fail-closed。
// gate=nullptr = 常驻，无配置开关；理由见 feature.h §「常开，无配置开关」——关闭只会把
// 已持有这些装备的角色重新锁住，且字符侧没有获取途径，开启不产生任何新行为。
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
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    set_item_facts(nullptr);
    set_character_facts(nullptr);
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    if (g_sdk->capabilities.require == nullptr) return 1;
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface)) ||
        iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    set_item_facts(static_cast<const contracts::ItemFactsApi*>(iface));
    iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kCharacterFactsCapabilityId,
            {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor},
            &iface)) ||
        iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        set_item_facts(nullptr);
        return 1;
    }
    set_character_facts(static_cast<const contracts::CharacterFactsApi*>(iface));
    // 常开，无配置开关：只置业务门禁并注册出售门禁谓词；资源由宿主声明式安装。
    // 依赖缺失即 fail-closed 返回非 0，宿主不安装资源并置 failed（与旧 install_if_ready
    // 的 deps_ready 门一致）。重复调用幂等（activate 重入）。
    if (!deps_ready()) {
        log_deps_missing();
        return 1;
    }
    set_gate(true);
    // 出售门禁登记到 hooks/dispatch 布尔门（resource_id = claim id）。fail-soft：登记失败
    // （已被他人占用/容量满，当前实际不可达）仅告警，不使 CanEquip/CanUnequip 主功能失败；
    // 代价 = 本次出售门禁不生效。activate 重入同 fn 幂等成功，不重复告警。
    if (g_sdk->hooks.set_intercept == nullptr ||
        g_sdk->hooks.set_intercept(kSellGateResourceId, &sell_gate_should_block, nullptr).ok !=
            kQolOk) {
        log_warn("sell gate intercept register rejected id=%s", kSellGateResourceId);
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    // 门禁置 false 并注销布尔门（幂等）；重复调用安全。
    set_gate(false);
    set_item_facts(nullptr);
    set_character_facts(nullptr);
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

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2, kRequires, // requires=item.facts + character.facts
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    2, kResources,
    0, nullptr,   // jni（常开无开关，无 JNI 绑定）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
};

}  // namespace

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolGameHost* game_host() { return g_game; }

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::special_equip
