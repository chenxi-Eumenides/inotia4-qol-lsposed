#include "plugin.h"

#include <cstdint>
#include <cstring>

#include "../../contracts/panels.h"
#include "../../contracts/http.h"
#include "../../contracts/text_layer.h"
#include "../../contracts/ui_choice.h"
#include "../../contracts/ui_confirmation.h"
#include "../../contracts/ui.h"
#include "feature.h"
#include "panels/debug_components.h"
#include "panels/debug_scroll.h"
#include "panels/debug_help.h"
#include "panels/stack.h"
#include "text_mechanism.h"
#include "ui_components.h"
#include "ui_confirmation.h"

namespace qol::features::core_ui {
namespace {

constexpr const char* kPluginId = "core.custom_ui";
constexpr const char* kPluginVersion = "0.1.1";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolChoicePort* g_choice_port = nullptr;

// 能力接口实例：函数地址为编译期常量；宿主进入 enabled 时发布。
constexpr contracts::UiPanelsApi kUiPanelsApi{
    &ui_panels_open,
    &ui_panels_close,
    &ui_panels_active,
    &ui_panels_block_reason,
    &ui_panels_deactivate_owner,
    &ui_panels_back,
};

constexpr contracts::UiChoiceApi kUiChoiceApi{
    &native_choice_open,
    &native_choice_active,
    &native_choice_close,
};

constexpr contracts::TextLayerApi kTextLayerApi{&text_api_subscribe, &text_api_unsubscribe};

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
    {
        contracts::kUiChoiceCapabilityId,
        {contracts::kUiChoiceVersionMajor, contracts::kUiChoiceVersionMinor},
        &kUiChoiceApi,
    },
    {
        contracts::kUiConfirmationCapabilityId,
        {contracts::kUiConfirmationVersionMajor, contracts::kUiConfirmationVersionMinor},
        ui_confirmation_api(),
    },
    {
        // 文本机制（ADR 0001 D3）：唯一 GetText 钩子 + 注册/分派；内容由各内容插件注册。
        contracts::kTextLayerCapabilityId,
        {contracts::kTextLayerVersionMajor, contracts::kTextLayerVersionMinor},
        &kTextLayerApi,
    },
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kUiStateCapabilityId,
     {contracts::kUiStateVersionMajor, contracts::kUiStateVersionMinor}},
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    {contracts::kHttpRegistryCapabilityId,
     {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId,
     {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor}},
#endif
};

// 原生选择控件的三条私有资源（ADR 0001：控件本体归工具插件，随 ui_choice 一起从
// core.ui_state 迁入）。均常驻（gate=nullptr）、私有、必需、fail-closed、可物理还原；
// 声明序 [init, button, draw] 与安装序一致，后项失败时宿主逆序回滚前项。
// draw 资源让模块 choice 首帧跳过游戏压暗（AddAlpha+FillRect+SaveLCD）。
const contracts::ResourceClaim kResources[] = {
    {
        "hook:UIChoice_Init",   // id
#if defined(QOL_ENABLE_DEVICE_PROBE) && QOL_ENABLE_DEVICE_PROBE
        "elf-export:UIChoice_Init", // device probe 使用严格导出解析。
#else
        "F_UI_CHOICE_INIT_VMA", // symbol（入口解析，game/access/symbols 登记）
#endif
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                   // resumable
        kPluginId,              // owner
        "选择框标题需在 UIChoice_Init 后改写为模块标题",  // reason
        nullptr,                // gate（常驻）
        120,                    // step_budget
        reinterpret_cast<void*>(&choice_init_wrapper),  // handler
    },
    {
        "ptrslot:g_uichoice_button_list_exe_got",  // id
        "ptrslot:g_uichoice_button_list_exe_got",  // symbol（经 game/access 指针槽登记解析）
        contracts::ResourceMechanism::kPtrSlot,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "选择按钮 ExecuteProc 槽：模块需截获选择索引以驱动业务回调",  // reason
        nullptr,
        120,
        reinterpret_cast<void*>(&choice_button_execute),  // handler
    },
    {
        "hook:Scene_Draw_POPUP_SC_CHOICE",  // id
#if defined(QOL_ENABLE_DEVICE_PROBE) && QOL_ENABLE_DEVICE_PROBE
        "elf-export:Scene_Draw_POPUP_SC_CHOICE",  // device probe 用严格导出解析。
#else
        "F_SCENE_DRAW_POPUP_SC_CHOICE_VMA",  // symbol（game/access/symbols 登记）
#endif
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                               // resumable
        kPluginId,                          // owner
        "模块 choice 首帧保存原始画面并跳过游戏压暗，避免深色面板下全黑",  // reason
        nullptr,                            // gate（常驻）
        120,                                // step_budget
        reinterpret_cast<void*>(&choice_draw_wrapper),  // handler
    },
    {
        // 文本机制：共享资源（§4.3/§4.5），多消费者经能力 subscribe，owner 独占同址 hook。
        kResourceIdGetText,        // id
        "F_MEMORYTEXT_GET_TEXT_VMA",  // symbol
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kShared,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                      // resumable
        kPluginId,                 // owner
        "全游戏唯一文本热点（232 调用点）；LSPosed 同址二次挂载失败，只能单一 owner + dispatch 分派",
        nullptr,                   // gate（常驻）
        120,                       // step_budget
        reinterpret_cast<void*>(&memorytext_get_text_wrapper),  // handler
    },
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHOICE,
     {contracts::kQolPortChoiceVersion.major, contracts::kQolPortChoiceVersion.minor}},
};

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
const contracts::HttpEndpointDescriptor kHttpEndpoints[] = {
    {contracts::HttpMethod::kPost, "/api/debug/custom-ui/components", kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr,
     &debug_components_http_command, nullptr},
    {contracts::HttpMethod::kPost, "/api/debug/custom-ui/scroll", kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr,
     &debug_scroll_http_command, nullptr},
    {contracts::HttpMethod::kPost, "/api/debug/custom-ui/help", kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr,
     &debug_help_http_command, nullptr},
};
#endif

void plugin_install_rollback(contracts::PluginContext& ctx, bool confirmation_bound,
                             bool debug_cleanup) {
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    if (debug_cleanup && ctx.sdk != nullptr) {
        debug_help_unregister(ctx.sdk);
        debug_scroll_unregister(ctx.sdk);
        debug_components_unregister(ctx.sdk);
    }
#else
    (void)debug_cleanup;
#endif
    if (confirmation_bound) ui_confirmation_unbind();
    text_set_active(false);
    text_mechanism_bind(nullptr);
    ui_panels_bind_sdk(nullptr);
    ui_components_unbind();
    g_choice_port = nullptr;
    g_sdk = nullptr;
}

// 界面工具/框架插件（ADR 0001 D1 的 A 档）：可复用组件、面板注册/导航、原生选择/确认控件
// 与文本机制。具体页面（设置页等）由各自插件拥有；Debug 构建额外注册组件探针。
int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_choice_port = nullptr;
    if (ctx.sdk == nullptr) {
        plugin_install_rollback(ctx, false, false);
        return 1;
    }
    if (!ui_components_bind(ctx.sdk->ui)) {
        plugin_install_rollback(ctx, false, false);
        return 1;
    }
    ui_panels_bind_sdk(ctx.sdk);
    text_mechanism_bind(ctx.sdk);
    // 选择控件需要游戏侧原语端口；缺失即 install 失败（宿主不启用、资源不装）。
    if (ctx.ports == nullptr || ctx.port_count < 1 || ctx.ports[0] == nullptr) {
        plugin_install_rollback(ctx, false, false);
        return 1;
    }
    g_choice_port = static_cast<const contracts::QolChoicePort*>(ctx.ports[0]);
    if (!ui_confirmation_bind(ctx.sdk->ui, ctx.game, ctx.sdk)) {
        plugin_install_rollback(ctx, false, false);
        return 1;
    }
    if (!ui_confirmation_register_builtin_frame_overlay()) {
        plugin_install_rollback(ctx, true, false);
        return 1;
    }
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    if (!debug_components_register(ctx.sdk)) {
        plugin_install_rollback(ctx, true, true);
        return 1;
    }
    if (!debug_scroll_register(ctx.sdk)) {
        plugin_install_rollback(ctx, true, true);
        return 1;
    }
    if (!debug_help_register(ctx.sdk)) {
        plugin_install_rollback(ctx, true, true);
        return 1;
    }
#endif
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(false);
    ui_confirmation_set_installed(false);
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    debug_help_unregister(ctx.sdk);
    debug_scroll_unregister(ctx.sdk);
    debug_components_unregister(ctx.sdk);
#endif
    text_set_active(false);
    text_mechanism_bind(nullptr);
    g_choice_port = nullptr;
    ui_panels_bind_sdk(nullptr);
    ui_confirmation_unbind();
    ui_components_unbind();
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(true);
    if (!ui_confirmation_set_installed(true) ||
        !ui_confirmation_register_builtin_frame_overlay()) {
        ui_confirmation_set_installed(false);
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            static constexpr char message[] = "confirmation overlay registration failed; disabled";
            (void)g_sdk->log.write(2, 12, "core.custom_ui/plugin", message,
                                   sizeof(message) - 1);
        }
    }
    text_set_active(true);
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    debug_components_set_enabled(true);
    debug_scroll_set_enabled(true);
    debug_help_set_enabled(true);
#endif
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(false);
    ui_confirmation_set_installed(false);
    text_set_active(false);
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    debug_components_set_enabled(false);
    debug_scroll_set_enabled(false);
    debug_help_set_enabled(false);
#endif
}

// 资源安装成功回调：按资源 id 分派给选择控件与文本机制各自的 backup 处理。
int32_t custom_ui_resource_ready(contracts::PluginContext& ctx, const char* resource_id,
                                 void* backup) {
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdGetText) == 0) {
        text_on_get_text_ready(backup);
        return 0;
    }
    return on_resource_ready(ctx, resource_id, backup);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;
    out.error = nullptr;
}

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
QolResult open_debug_components_feature(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr) {
        constexpr char message[] = "debug panel navigation unavailable";
        return contracts::error(kQolErrNotReady, message, sizeof(message) - 1);
    }
    if (ui_panels_open("debug_components")) return contracts::ok();
    constexpr char message[] = "debug panel navigation failed";
    return contracts::error(kQolErrBlocked, message, sizeof(message) - 1);
}
#endif

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"core.custom_ui.ui_tools", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "共享界面工具", "",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    {"core.custom_ui.open_debug_components", 1, contracts::FeatureCategory::kHidden,
       contracts::FeatureType::kEntry, "打开调试面板", "",
       nullptr, &open_debug_components_feature, "打开", -5},
#endif
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,      // requires
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires, // ui.state and optional Debug HTTP capabilities
    5, kProvides,    // provides（ui_components / ui_panels / text_layer / ui_confirmation，ui_choice）
    4, kResources,   // resources（选择控件 3 条 + 文本 GetText 共享资源）
    0, nullptr,      // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &custom_ui_resource_ready,
    kPorts,
    1,
    nullptr,         // deprecated http_queries（Debug commands are typed endpoints below）
    0,
    kFeatures,
    static_cast<uint32_t>(sizeof(kFeatures) / sizeof(kFeatures[0])),
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    kHttpEndpoints, static_cast<uint32_t>(sizeof(kHttpEndpoints) / sizeof(kHttpEndpoints[0])),
#else
    nullptr, 0,
#endif
    nullptr,
    nullptr, 0,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolChoicePort* choice_port() { return g_choice_port; }

}  // namespace qol::features::core_ui
