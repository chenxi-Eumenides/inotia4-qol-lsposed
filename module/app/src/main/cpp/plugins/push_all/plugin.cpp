#include "plugins/push_all/plugin.h"

#include <cstdint>
#include <cstring>

#include "contracts/events.h"
#include "core/native/qol_log.h"
#include "plugins/push_all/feature.h"
#include "infra/config.h"
#include "infra/events.h"

namespace qol::features::push_all {
namespace {

constexpr const char* kPluginId = "push_all";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kGateKey = "plugin.push_all.enabled";

// 配置变更订阅句柄；deactivate 时成对注销。
infra::SubscriptionId g_config_subscription = 0;

void on_config_changed(void* ctx, const void* payload) {
    (void)ctx;
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::ConfigChangedEvent*>(payload);
    if (event->key == nullptr || std::strcmp(event->key, kGateKey) != 0) return;
    set_gate(event->int_value != 0);
}

// 两条资源均为调用点 patch、私有、必需、fail-closed、可物理还原。
// 资源声明（ARCHITECTURE.md §4.3 字段顺序）。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdParty,          // id
        "P_CHAR_MOVE_PUSH_PARTY",  // symbol：patch 点登记名
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                      // resumable
        kPluginId,                 // owner
        "队伍闸门是内联 BL 调用点，无函数指针槽可挂，只能重定向到 wrapper",  // reason
        kGateKey,                  // gate
        120,                       // step_budget
        reinterpret_cast<void*>(&push_all_party_filter),  // handler（BL 目标）
    },
    {
        kResourceIdStand,
        "P_CHAR_MOVE_PUSH_STAND",
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "站立检查是条件分支，无可透传执行体，只能将 cbz 置 NOP；禁用即物理还原",
        kGateKey,
        120,
        nullptr,  // kWordReplace 无 handler
    },
};

// JNI 绑定元数据：符号名与签名在重构期冻结；签名哈希 P5 落地前为 nullptr。
const contracts::JniBinding kJni[] = {
    {
        "nativeSetPushAllEnabled",
        "Java_com_inotia4_qol_NativeBridge_nativeSetPushAllEnabled",
        nullptr,
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // inactive → resolving 是幂等重入（§3.2）：门禁读取可重复，订阅已有则不重复订阅。
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

}  // namespace qol::features::push_all
