#include "plugin.h"

#include <cstdint>

#include "../../contracts/http.h"
#include "../../contracts/ownership.h"
#include "../../contracts/panels.h"
#include "feature.h"
#include "panels/settings.h"
#include "panels/stack.h"
#include "ui_components.h"

#include <cstring>
#include <string>

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

// ---- HTTP command handlers（/api/debug/settings-ui/* POST，owner=core.ui）----
// 四条端点均无请求体：插件内业务函数直调，返回体逐字与迁移前 JNI 透传一致。
// 自注册模式（core.api lifecycle 先例）：停用由宿主 unregister_http_queries 摘除置墓碑。

QolResult settings_ui_command_body(contracts::HttpRes* response,
                                                   const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        if (response != nullptr && response->body != nullptr) {
            static constexpr char kMsg[] = "{\"ok\":false,\"error\":\"http response buffer too small\"}";
            const uint32_t len = static_cast<uint32_t>(sizeof(kMsg) - 1);
            std::memcpy(response->body, kMsg, len);
            response->body_size = len;
            response->status = 500;
        }
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

QolResult settings_ui_inject_command(const contracts::HttpReq*, contracts::HttpRes* response,
                                                     void*) {
    return settings_ui_command_body(response, data_settings_ui_inject());
}

QolResult settings_ui_restore_command(const contracts::HttpReq*, contracts::HttpRes* response,
                                                      void*) {
    return settings_ui_command_body(response, data_settings_ui_restore());
}

QolResult settings_ui_open_option_command(const contracts::HttpReq*,
                                                          contracts::HttpRes* response, void*) {
    return settings_ui_command_body(response, data_settings_ui_open_option());
}

QolResult settings_ui_open_panel_command(const contracts::HttpReq*,
                                                         contracts::HttpRes* response, void*) {
    return settings_ui_command_body(response, data_settings_ui_open_panel());
}

struct SettingsCommandRoute {
    const char* path;
    QolResult (*handler)(const contracts::HttpReq*, contracts::HttpRes*, void*);
};

constexpr SettingsCommandRoute kSettingsCommandRoutes[] = {
    {"/api/debug/settings-ui/inject", &settings_ui_inject_command},
    {"/api/debug/settings-ui/restore", &settings_ui_restore_command},
    {"/api/debug/settings-ui/open-option", &settings_ui_open_option_command},
    {"/api/debug/settings-ui/open-panel", &settings_ui_open_panel_command},
};

bool register_settings_ui_commands(const contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->http_dispatch.register_command == nullptr) return false;
    for (const SettingsCommandRoute& route : kSettingsCommandRoutes) {
        const QolResult result =
            sdk->http_dispatch.register_command(contracts::HttpMethod::kPost, route.path,
                                                kPluginId, route.handler, nullptr);
        if (!contracts::succeeded(result)) {
            if (sdk->http_dispatch.unregister_owner != nullptr) {
                (void)sdk->http_dispatch.unregister_owner(kPluginId);
            }
            return false;
        }
    }
    return true;
}

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || !ui_components_bind(ctx.sdk->ui)) return 1;
    panels_bind_sdk(ctx.sdk);
    // install 只做自身初始化：注册设置页 provider + 存档备份动作项 + 按钮绑定帧任务。
    // 资源由宿主声明式安装，backup 经 on_resource_ready 交还。
    if (!panels_register()) return 1;
    // HTTP command 注册在初始化完成后执行；失败即 install 失败（宿主不启用、资源不装）。
    if (!register_settings_ui_commands(ctx.sdk)) return 1;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    // 自注册的 command handler 不在 descriptor（http_query_count=0），宿主的
    // unregister_http_queries 会早退；deactivate 必须显式摘除（幂等，core.api 同款）。
    if (ctx.sdk != nullptr && ctx.sdk->http_dispatch.unregister_owner != nullptr) {
        (void)ctx.sdk->http_dispatch.unregister_owner(kPluginId);
    }
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
