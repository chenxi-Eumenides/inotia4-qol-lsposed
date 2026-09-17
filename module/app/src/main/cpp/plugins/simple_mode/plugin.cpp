#include "plugins/simple_mode/plugin.h"

#include <cstdint>
#include <cstring>

#include "contracts/events.h"
#include "core/native/qol_log.h"
#include "plugins/simple_mode/feature.h"
#include "infra/config.h"
#include "infra/events.h"

namespace qol::features::simple_mode {
namespace {

constexpr const char* kPluginId = "simple_mode";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kGateKey = "plugin.simple_mode.enabled";

// 配置变更订阅句柄；deactivate 时成对注销。
infra::SubscriptionId g_config_subscription = 0;

void on_config_changed(void* ctx, const void* payload) {
    (void)ctx;
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::ConfigChangedEvent*>(payload);
    if (event->key == nullptr || std::strcmp(event->key, kGateKey) != 0) return;
    set_gate(event->int_value != 0);
}

// 资源声明（ARCHITECTURE.md §4.3 字段顺序）：两个入口 hook 均为私有、必需、fail-closed。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdAddDamage,       // id
        "F_CHAR_ADD_DAMAGE_VMA",    // symbol
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                       // resumable
        kPluginId,                  // owner
        "无函数指针槽，15 个静态调用点经此单点汇合",  // reason
        kGateKey,                   // gate
        120,                        // step_budget
        reinterpret_cast<void*>(&char_add_damage_wrapper),  // handler
    },
    {
        kResourceIdUpdateAttrFromMonster,
        "F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "最大生命重算唯一入口",
        kGateKey,
        120,
        reinterpret_cast<void*>(&char_update_attr_from_monster_wrapper),
    },
};

// JNI 绑定元数据：符号名与签名在重构期冻结；签名哈希 P5 落地前为 nullptr。
const contracts::JniBinding kJni[] = {
    {
        "nativeSetSimpleModeEnabled",
        "Java_com_inotia4_qol_NativeBridge_nativeSetSimpleModeEnabled",
        nullptr,
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // inactive → resolving 是幂等重入（§3.2）：门禁读取可重复执行，订阅已有则不重复订阅
    // （重复订阅会在 deactivate 退订后留下孤儿订阅）。
    set_gate(infra::config_get_bool(kGateKey, false));
    if (g_config_subscription == 0) {
        g_config_subscription =
            infra::subscribe(contracts::kEventConfigChanged, &on_config_changed, nullptr);
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    // 幂等：退订后置 0，便于再次 install 重新订阅；重复调用安全。
    if (g_config_subscription != 0) {
        infra::unsubscribe(g_config_subscription);
        g_config_subscription = 0;
    }
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
    1, kJni,
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

}  // namespace qol::features::simple_mode
