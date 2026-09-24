#include "plugin.h"

#include <cstdint>

#include "../../contracts/panels.h"
#include "panels/stack.h"
#include "ui_components.h"

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

// 界面工具/框架插件：只提供可复用组件与面板注册/导航能力（ADR 0001 D1 的 A 档）。
// 具体页面（设置页等）由各自插件拥有；本插件不注册任何 HTTP 路由、不持资源。
int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || !ui_components_bind(ctx.sdk->ui)) return 1;
    ui_panels_bind_sdk(ctx.sdk);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    ui_panels_bind_sdk(nullptr);
    ui_components_unbind();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,      // requires
    0, nullptr,      // optional_requires
    2, kProvides,    // provides（ui_components/ui_panels v1.0）
    0, nullptr,      // resources（页面资源归各自页面插件）
    0, nullptr,      // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,         // on_resource_ready
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_ui
