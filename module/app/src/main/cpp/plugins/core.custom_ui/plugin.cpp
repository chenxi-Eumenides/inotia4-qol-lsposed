#include "plugin.h"

#include <cstdint>
#include <cstring>

#include "../../contracts/panels.h"
#include "../../contracts/text_layer.h"
#include "../../contracts/ui_choice.h"
#include "feature.h"
#include "panels/stack.h"
#include "text_mechanism.h"
#include "ui_components.h"

namespace qol::features::core_ui {
namespace {

constexpr const char* kPluginId = "core.custom_ui";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolChoicePort* g_choice_port = nullptr;

// 能力接口实例：函数地址为编译期常量；宿主进入 enabled 时发布。
constexpr contracts::UiPanelsApi kUiPanelsApi{
    &ui_panels_open,
    &ui_panels_close,
    &ui_panels_active,
    &ui_panels_block_reason,
    &ui_panels_deactivate_owner,
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
        // 文本机制（ADR 0001 D3）：唯一 GetText 钩子 + 注册/分派；内容由各内容插件注册。
        contracts::kTextLayerCapabilityId,
        {contracts::kTextLayerVersionMajor, contracts::kTextLayerVersionMinor},
        &kTextLayerApi,
    },
};

// 原生选择控件的两条私有资源（ADR 0001：控件本体归工具插件，随 ui_choice 一起从
// core.ui_state 迁入）。2 条资源均常驻（gate=nullptr）、私有、必需、fail-closed、
// 可物理还原；声明序 [init, button] 与旧安装序一致，按钮槽失败时宿主逆序回滚 init。
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

// 界面工具/框架插件（ADR 0001 D1 的 A 档）：可复用组件、面板注册/导航、原生选择控件
// 与文本机制。具体页面（设置页等）由各自插件拥有；本插件不注册 HTTP 路由。
int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_choice_port = nullptr;
    if (ctx.sdk == nullptr || !ui_components_bind(ctx.sdk->ui)) return 1;
    ui_panels_bind_sdk(ctx.sdk);
    text_mechanism_bind(ctx.sdk);
    // 选择控件需要游戏侧原语端口；缺失即 install 失败（宿主不启用、资源不装）。
    if (ctx.ports == nullptr || ctx.port_count < 1 || ctx.ports[0] == nullptr) return 1;
    g_choice_port = static_cast<const contracts::QolChoicePort*>(ctx.ports[0]);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(false);
    text_set_active(false);
    text_mechanism_bind(nullptr);
    g_choice_port = nullptr;
    ui_panels_bind_sdk(nullptr);
    ui_components_unbind();
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(true);
    text_set_active(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(false);
    text_set_active(false);
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

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,      // requires
    0, nullptr,      // optional_requires
    4, kProvides,    // provides（ui_components / ui_panels / ui_choice / text_layer v1.0）
    3, kResources,   // resources（选择控件 2 条 + 文本 GetText 共享资源）
    0, nullptr,      // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &custom_ui_resource_ready,
    kPorts,
    1,
    nullptr,         // http_queries（本插件不注册路由）
    0,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolChoicePort* choice_port() { return g_choice_port; }

}  // namespace qol::features::core_ui
