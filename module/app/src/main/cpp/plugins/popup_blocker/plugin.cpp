#include "plugin.h"

#include <atomic>
#include <cstdint>

// 保留教程状态入队与 UIPlayPorting_Draw 的 state 写入。TutorialDescription entry hook 直接
// 返回 0，不运行原文案绘制函数，并关闭由其非零返回值控制的关联提示。state=6 的 pause
// 门闩在 UIPlayPorting_Draw+0xcf4 写入；此处 NOP 写入，不先设置 pause 再调用 tutorial_cancel。
// game_state.cpp 的 ui_screen()/tutorial_block_error() 仍只负责旧状态清理，不是本插件的
// 暂停抑制路径。P_HUB_LOGIN_SKIP 是 Host 常驻 patch，不属于本插件。

namespace qol::features::popup_blocker {
namespace {

constexpr const char* kPluginId = "popup_blocker";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kTutorialDescriptionResource = "hook:TutorialDescription";

std::atomic<bool> g_enabled{false};
std::atomic<const contracts::QolHostSdk*> g_sdk{nullptr};

int32_t tutorial_description_suppress() {
    const contracts::QolHostSdk* sdk = g_sdk.load(std::memory_order_acquire);
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kTutorialDescriptionResource});
    (void)guard;
    return 0;
}

// 先 NOP pause store，避免安装期间 state=6 先打开暂停门闩；同组失败由 Host 原子回滚。
const contracts::ResourceClaim kResources[] = {
    {"P_TUTORIAL_PAUSE_GATE_SKIP", "P_TUTORIAL_PAUSE_GATE_SKIP",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailClosed,
     true,
     kPluginId,
     "UIPlayPorting_Draw 保留 state=6 写入，但不设置教学暂停门闩",
     nullptr,
     120,
     nullptr,
     true},
    {kTutorialDescriptionResource, "F_TUTORIAL_DESCRIPTION_VMA",
     contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailClosed,
     true,
     kPluginId,
     "短路 TutorialDescription 原函数，抑制文案绘制并返回 0",
     nullptr,
     120,
     reinterpret_cast<void*>(&tutorial_description_suppress),
     true},
};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) || ctx.sdk == nullptr ||
        ctx.game == nullptr) {
        g_sdk.store(nullptr, std::memory_order_release);
        return 1;
    }
    g_sdk.store(ctx.sdk, std::memory_order_release);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(false, std::memory_order_release);
    g_sdk.store(nullptr, std::memory_order_release);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(true, std::memory_order_release);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(false, std::memory_order_release);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_enabled.load(std::memory_order_acquire);
    out.error = nullptr;
}

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"popup_blocker.enabled", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "禁用弹窗",
     "禁用各种无用弹窗",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,  // requires
    0, nullptr,  // optional_requires
    0, nullptr,  // provides
    static_cast<uint32_t>(sizeof(kResources) / sizeof(kResources[0])), kResources,
    0, nullptr,  // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,     // on_resource_ready
    nullptr, 0,  // ports
    nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::popup_blocker
