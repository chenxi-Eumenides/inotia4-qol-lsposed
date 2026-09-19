#include "plugins/core.ui/plugin.h"

#include <cstdint>

#include "contracts/ownership.h"
#include "contracts/panels.h"
#include "plugins/core.ui/feature.h"
#include "plugins/core.ui/panels/settings.h"
#include "plugins/core.ui/panels/stack.h"
#include "plugins/core.ui/ui_components.h"

namespace qol::features::core_ui {
namespace {

constexpr const char* kPluginId = "core.ui";
constexpr const char* kPluginVersion = "0.1.0";

// 能力接口实例：函数地址为编译期常量；宿主进入 enabled 时发布。
constexpr contracts::UiPanelsApi kUiPanelsApi{
    &ui_panels_open,
    &ui_panels_close,
    &ui_panels_active,
    &ui_panels_block_reason,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {
        contracts::kUiComponentsCapabilityId,
        {contracts::kUiContractMajor, contracts::kUiContractMinor},
        ui_components_api(),
    },
    {
        contracts::kUiPanelsCapabilityId,
        {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor},
        &kUiPanelsApi,
    },
};

// own:popup.settings 的 OwnershipOps：acquire/release 走 hooks/popup_state 原语。
const contracts::OwnershipOps kSettingsOwnershipOps{
    &settings_popup_acquire,
    &settings_popup_release,
};

const contracts::ResourceClaim kResources[] = {
    {
        "own:popup.settings",  // id
        "own:popup.settings",  // symbol（kOwnership 不使用站点解析，保留 id 作诊断标识）
        contracts::ResourceMechanism::kOwnership,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                  // resumable
        kPluginId,             // owner
        "设置面板以 PopupState 死条目承载，需整段占用 5 回调并承诺写回快照",  // reason
        nullptr,               // gate（常驻）
        120,                   // step_budget
        reinterpret_cast<void*>(const_cast<contracts::OwnershipOps*>(&kSettingsOwnershipOps)),  // handler
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || !ui_components_bind(ctx.sdk->ui)) return 1;
    panels_bind_sdk(ctx.sdk);
    // install 只做自身初始化：注册设置页 provider + 存档备份动作项 + 按钮绑定帧任务。
    // 资源由宿主声明式安装，backup 经 on_resource_ready 交还。
    if (!panels_register()) return 1;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    panels_unregister();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    plugin_set_enabled(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    plugin_set_enabled(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = plugin_enabled();
    // 暴露「更多游戏」按钮绑定状态（waiting/live/stale/error）：未登记 PtrHook 不得
    // 被隐藏成完全受管资源，状态面必须可见。
    out.error = settings_more_games_bind_state();
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,      // requires
    0, nullptr,      // optional_requires
    2, kProvides,    // provides（ui_components/ui_panels v1.0）
    1, kResources,   // resources（own:popup.settings）
    0, nullptr,      // jni（JNI 入口由 bridge/native/gamebridge_settings.cpp 保留）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,         // on_resource_ready（kOwnership 无 backup）
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_ui
