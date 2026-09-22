#include "plugin.h"

#include <cstdint>

#include "../../contracts/ui_choice.h"
#include "feature.h"

namespace qol::features::native_choice {
namespace {

constexpr const char* kPluginId = "native_choice";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolChoicePort* g_choice_port = nullptr;

// 能力接口实例：函数地址为编译期常量，无跨 TU 静态初始化顺序问题；宿主进入 enabled 时注册。
constexpr contracts::UiChoiceApi kUiChoiceApi{
    &native_choice_open,
    &native_choice_active,
    &native_choice_close,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {
        contracts::kUiChoiceCapabilityId,
        {contracts::kUiChoiceVersionMajor, contracts::kUiChoiceVersionMinor},
        &kUiChoiceApi,
    },
};

// 2 条资源均常驻（gate=nullptr）、私有、必需、fail-closed、可物理还原；声明序 [init, button]
// 与旧安装序一致，按钮槽失败时宿主逆序回滚 init（等价旧手动 unhook）。
const contracts::ResourceClaim kResources[] = {
    {
        "hook:UIChoice_Init",   // id
        "F_UI_CHOICE_INIT_VMA", // symbol（入口解析，game/access/symbols 登记）
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
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHOICE,
     {contracts::kQolPortChoiceVersion.major, contracts::kQolPortChoiceVersion.minor}},
};

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_choice_port = nullptr;
    if (g_sdk == nullptr || ctx.ports == nullptr || ctx.port_count < 1 ||
        ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_choice_port = static_cast<const contracts::QolChoicePort*>(ctx.ports[0]);
    // 自身初始化无额外动作：资源由宿主声明式安装，backup 经 on_resource_ready 交还。
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(false);
    g_choice_port = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    native_choice_set_installed(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;  // 零门禁常驻
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    1, kProvides, // provides（ui_choice v1.0）
    2, kResources,
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    kPorts,
    1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolChoicePort* choice_port() { return g_choice_port; }

}  // namespace qol::features::native_choice
