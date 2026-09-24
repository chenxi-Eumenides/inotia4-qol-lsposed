#include "plugin.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "../../contracts/http.h"
#include "../../contracts/ownership.h"
#include "../../contracts/panels.h"
#include "../../contracts/ui.h"
#include "feature.h"
#include "panels/settings.h"

namespace qol::features::core_settings {
namespace {

constexpr const char* kPluginId = "core.settings";
constexpr const char* kPluginVersion = "0.1.0";
constexpr uint32_t kConfigResponseCapacity = 8192;
constexpr uint32_t kConfigStringCapacity = 4096;
const contracts::QolHostSdk* g_sdk = nullptr;

// 设置页只消费工具能力，不提供能力（ADR 0001 D2：能力 ID/版本保持原样，改的只是提供者）。
const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kUiPanelsCapabilityId,
     {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}},
    {contracts::kUiComponentsCapabilityId,
     {contracts::kUiContractMajor, contracts::kUiContractMinor}},
    {contracts::kHttpRegistryCapabilityId,
     {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId,
     {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor}},
};

// own:popup.settings 的 OwnershipOps：acquire/release 走 hooks/popup_state 原语。
const contracts::OwnershipOps kSettingsOwnershipOps{
    &core_ui::settings_popup_acquire,
    &core_ui::settings_popup_release,
};

void append_json_string(std::string& out, const char* value) {
    out.push_back('"');
    for (const unsigned char ch : std::string(value == nullptr ? "" : value)) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[ch >> 4];
                    out += kHex[ch & 0x0f];
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

QolResult config_list_query(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    if (response == nullptr || response->body == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid response", 16);
    }
    // get_string 自 Host SDK minor 10 起提供；消费前按 abi.minor 协商。
    if (g_sdk == nullptr || g_sdk->abi.minor < 10 || g_sdk->config.get_string == nullptr ||
        g_sdk->config.get_i32 == nullptr || g_sdk->config.get_bool == nullptr) {
        return contracts::error(kQolErrNotReady, "configuration unavailable", 26);
    }
    char address[kConfigStringCapacity]{};
    uint32_t address_size = 0;
    int32_t port = 8088;
    if (!contracts::succeeded(g_sdk->config.get_string("listenAddress", "0.0.0.0", address,
                                                        sizeof(address), &address_size)) ||
        address_size >= sizeof(address) ||
        !contracts::succeeded(g_sdk->config.get_i32("listenPort", 8088, &port))) {
        return contracts::error(kQolErrNotReady, "configuration unavailable", 26);
    }
    auto get_bool = [](const char* key, bool fallback, bool* out) {
        if (g_sdk == nullptr || g_sdk->config.get_bool == nullptr || out == nullptr) return false;
        int32_t value = fallback ? 1 : 0;
        if (!contracts::succeeded(g_sdk->config.get_bool(key, value, &value))) return false;
        *out = value != 0;
        return true;
    };
    bool stack_limit = false;
    bool move_merge = false;
    bool op_enabled = false;
    bool extension_bag = true;
    bool custom_recipe = false;
    bool simple_mode = false;
    bool push_all = false;
    bool auto_sell = false;
    bool api_enabled = true;
    bool debug_log = false;
    if (!get_bool("stackLimitIncrease", false, &stack_limit) ||
        !get_bool("moveMergeEnabled", false, &move_merge) ||
        !get_bool("opEnabled", false, &op_enabled) ||
        !get_bool("extensionBagEnabled", true, &extension_bag) ||
        !get_bool("customRecipeEnabled", false, &custom_recipe) ||
        !get_bool("simpleModeEnabled", false, &simple_mode) ||
        !get_bool("pushAllEnabled", false, &push_all) ||
        !get_bool("autoSellEnabled", false, &auto_sell) ||
        !get_bool("apiEnabled", true, &api_enabled) ||
        !get_bool("debugLogEnabled", false, &debug_log)) {
        return contracts::error(kQolErrNotReady, "configuration unavailable", 26);
    }
    std::string body = "{\"listenAddress\":";
    append_json_string(body, address);
    body += ",\"listenPort\":" + std::to_string(port);
    body += ",\"stackLimitIncrease\":";
    body += stack_limit ? "true" : "false";
    body += ",\"moveMergeEnabled\":";
    body += move_merge ? "true" : "false";
    body += ",\"opEnabled\":";
    body += op_enabled ? "true" : "false";
    body += ",\"extensionBagEnabled\":";
    body += extension_bag ? "true" : "false";
    body += ",\"customRecipeEnabled\":";
    body += custom_recipe ? "true" : "false";
    body += ",\"simpleModeEnabled\":";
    body += simple_mode ? "true" : "false";
    body += ",\"pushAllEnabled\":";
    body += push_all ? "true" : "false";
    body += ",\"autoSellEnabled\":";
    body += auto_sell ? "true" : "false";
    body += ",\"apiEnabled\":";
    body += api_enabled ? "true" : "false";
    body += ",\"debugLogEnabled\":";
    body += debug_log ? "true" : "false";
    body.push_back('}');
    if (response->body_capacity < body.size() || body.size() > kConfigResponseCapacity) {
        static constexpr char kError[] = "{\"ok\":false,\"error\":\"http response buffer too small\"}";
        if (response->body_capacity >= sizeof(kError) - 1) {
            std::memcpy(response->body, kError, sizeof(kError) - 1);
            response->body_size = sizeof(kError) - 1;
            response->status = 500;
        }
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

const contracts::HttpQueryHandlerDescriptor kHttpQueries[] = {
    {contracts::HttpMethod::kGet, "/api/config/list", kPluginId, &config_list_query, nullptr},
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
        reinterpret_cast<void*>(const_cast<contracts::OwnershipOps*>(&kSettingsOwnershipOps)),
    },
};

// ---- HTTP command handlers（/api/debug/settings-ui/* POST）----
// 四条端点均无请求体：插件内业务函数直调，返回体逐字与迁移前一致。
// 自注册模式（core.api lifecycle 先例）：停用由 deactivate 显式 unregister_owner 摘除置墓碑。

QolResult settings_ui_command_body(contracts::HttpRes* response, const std::string& body) {
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
    return settings_ui_command_body(response, core_ui::data_settings_ui_inject());
}

QolResult settings_ui_restore_command(const contracts::HttpReq*, contracts::HttpRes* response,
                                      void*) {
    return settings_ui_command_body(response, core_ui::data_settings_ui_restore());
}

QolResult settings_ui_open_option_command(const contracts::HttpReq*, contracts::HttpRes* response,
                                          void*) {
    return settings_ui_command_body(response, core_ui::data_settings_ui_open_option());
}

QolResult settings_ui_open_panel_command(const contracts::HttpReq*, contracts::HttpRes* response,
                                         void*) {
    return settings_ui_command_body(response, core_ui::data_settings_ui_open_panel());
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
    if (ctx.sdk == nullptr) return 1;
    g_sdk = ctx.sdk;
    core_ui::panels_bind_sdk(ctx.sdk);
    // install 只做自身初始化：注册设置页 provider + 存档备份动作项 + 按钮绑定帧任务。
    if (!core_ui::panels_register()) return 1;
    if (!register_settings_ui_commands(ctx.sdk)) return 1;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    // 自注册的 command handler 不在 descriptor（http_query_count=0），宿主的
    // unregister_http_queries 会早退；deactivate 必须显式摘除（幂等，core.api 同款）。
    if (ctx.sdk != nullptr && ctx.sdk->http_dispatch.unregister_owner != nullptr) {
        (void)ctx.sdk->http_dispatch.unregister_owner(kPluginId);
    }
    core_ui::panels_unregister();
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    core_ui::plugin_set_enabled(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    core_ui::plugin_set_enabled(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = core_ui::plugin_enabled();
    // 暴露「更多游戏」按钮绑定状态（waiting/live/stale/error）：未登记 PtrHook 不得
    // 被隐藏成完全受管资源，状态面必须可见。
    out.error = core_ui::settings_more_games_bind_state();
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    4, kRequires,    // requires（ui_panels + ui_components + http.registry + http.dispatch）
    0, nullptr,      // optional_requires
    0, nullptr,      // provides（页面不提供能力）
    1, kResources,   // resources（own:popup.settings）
    0, nullptr,      // jni（JNI 入口由 bridge/native/gamebridge_settings.cpp 保留）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,         // on_resource_ready（kOwnership 无 backup）
    nullptr,         // ports
    0,               // port_count
    kHttpQueries,
    static_cast<uint32_t>(sizeof(kHttpQueries) / sizeof(kHttpQueries[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_settings
