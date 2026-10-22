#include "plugin.h"

#include <atomic>
#include <cstdint>

// 教学状态由 Tutorialaddstate(long) 压入 5 槽轮转队列；Tutorialgetstate 消费队列状态，
// TutorialDescription 只在教学态由 UIPlayPorting_Draw 绘制对应提示文案。将入队入口替换为
// RET 后，状态不入队，因此教程提示和教学暂停都不会触发，TutorialDescription 也不会被调用。
// 安全性依据：空队列返回教学终态 99（或 2=教学完成），与 tutorial_cancel() 的终态一致；
// Tutorialpotioncheck 只设置教学 HUD 高亮标志，不门控治疗或战斗。存量存档残留仍由
// game_state.cpp 的 tutorial_cancel() 兜底。P_HUB_LOGIN_SKIP 是 Host 常驻 patch，不属于本插件。

namespace qol::features::popup_blocker {
namespace {

constexpr const char* kPluginId = "popup_blocker";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kReason =
    "教学状态入队入口 RET：避免新手教程提示与教学暂停触发";

// gate=nullptr：claim 随插件 lifecycle 安装/还原；单 feature 的 Host 生命周期决定运行状态。
const contracts::ResourceClaim kResources[] = {
    {"P_TUTORIAL_ADDSTATE_SKIP", "P_TUTORIAL_ADDSTATE_SKIP",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailClosed,
     true,
     kPluginId,
     kReason,
     nullptr,
     120,
     nullptr},
};

std::atomic<bool> g_enabled{false};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) || ctx.sdk == nullptr ||
        ctx.game == nullptr) {
        return 1;
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(false, std::memory_order_release);
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
     contracts::FeatureType::kToggle, "弹窗屏蔽", "屏蔽新手教程提示与由教学状态触发的教学暂停。",
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
