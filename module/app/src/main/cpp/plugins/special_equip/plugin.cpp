#include "plugins/special_equip/plugin.h"

#include <cstdint>

#include "core/native/qol_log.h"
#include "plugins/special_equip/feature.h"

// 过渡依赖（B2 拆分 inventory 域后清除）：出售按钮 UIEquip_ButtonDestroyExe 已被 legacy
// inventory hook 单点占用（同址 NativeHook 二次挂载 rc=-1），本插件只能注册谓词。
// 此处前置声明注册入口而不 include feature/patch/native_inventory_hook.h：该头归属
// plugins/patch，include 会形成 feature→feature 层依赖（新增架构违规）。B2 拆分 inventory
// 域后改为共享资源分派消费者，此声明随之删除。
void inventory_native_hook_set_sell_gate(bool (*predicate)());

namespace qol::features::special_equip {
namespace {

constexpr const char* kPluginId = "special_equip";
constexpr const char* kPluginVersion = "0.1.0";

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
    (void)ctx;
    // 常开，无配置开关：只置业务门禁并注册出售门禁谓词；资源由宿主声明式安装。
    // 依赖缺失即 fail-closed 返回非 0，宿主不安装资源并置 failed（与旧 install_if_ready
    // 的 deps_ready 门一致）。重复调用幂等（activate 重入）。
    if (!deps_ready()) {
        log_deps_missing();
        return 1;
    }
    set_gate(true);
    inventory_native_hook_set_sell_gate(&sell_gate_should_block);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    // 门禁置 false；谓词无注销接口，常驻但入口先判门禁即放行。重复调用幂等。
    set_gate(false);
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
    0, nullptr,   // requires
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

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::special_equip
