#include "plugin.h"

#include <cstdint>
#include <cstring>

#include "../../contracts/events.h"
#include "../../contracts/character_facts.h"
#include "feature.h"

namespace qol::features::push_all {
namespace {

constexpr const char* kPluginId = "push_all";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kGateKey = "plugin.push_all.enabled";

// 配置变更订阅句柄；deactivate 时成对注销。
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
uint64_t g_config_subscription = 0;

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kCharacterFactsCapabilityId,
     {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor}},
};

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
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    if (g_sdk->capabilities.require == nullptr) return 1;
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kCharacterFactsCapabilityId,
            {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor},
            &iface)) ||
        iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    set_character_facts(static_cast<const contracts::CharacterFactsApi*>(iface));
    // inactive → resolving 是幂等重入（§3.2）：门禁读取可重复，订阅已有则不重复订阅。
    int32_t enabled = 0;
    if (g_sdk->config.get_bool != nullptr) {
        (void)g_sdk->config.get_bool(kGateKey, 0, &enabled);
    }
    set_gate(enabled != 0);
    if (g_config_subscription == 0 && g_sdk->events.subscribe != nullptr) {
        (void)g_sdk->events.subscribe(contracts::kEventConfigChanged, &on_config_changed, nullptr,
                                       &g_config_subscription);
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    // 幂等：退订后置 0，便于再次 install 重新订阅；重复调用安全。
    if (g_config_subscription != 0 && g_sdk != nullptr && g_sdk->events.unsubscribe != nullptr) {
        (void)g_sdk->events.unsubscribe(g_config_subscription);
        g_config_subscription = 0;
    }
    set_gate(false);
    set_character_facts(nullptr);
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
    1, kRequires, // requires=character.facts
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

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolGameHost* game_host() { return g_game; }

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::push_all
