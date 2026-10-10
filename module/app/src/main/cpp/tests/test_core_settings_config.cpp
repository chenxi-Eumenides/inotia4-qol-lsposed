#include "contracts/http.h"
#include "contracts/host_sdk.h"
#include "contracts/plugin.h"
#include "plugins/core.settings/plugin.h"
#include "plugins/core.settings/panels/settings.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// core.settings 当前插件面（Host 常驻 config 路由重构后）：
// - descriptor 只声明 features=[page entry]，http_queries=0，resources=0；
// - /api/config/list|set|receipt、/api/op/features/lifecycle 由宿主常驻
//   host/features/http_control.cpp 实现，不再由本插件注册；
// - install 首行要求 feature_control 可用，随后只注册四条 /api/debug/settings-ui/*
//   POST 命令，deactivate 显式 unregister_owner("settings")。
// config/list 的字段顺序、类型与 JSON 转义覆盖已迁移到 host_feature_controller_tests
// 的 registered_projection 用例（测 controller.read_registered 序列化源）以及
// production_feature_facade_tests 的 /api/config/list HTTP 投影断言。

namespace {
int g_bind_sdk_calls = 0;
int g_panels_register_calls = 0;
int g_panels_unregister_calls = 0;
int g_provider_open_calls = 0;
int g_panels_open_calls = 0;
int g_inject_calls = 0;
int g_restore_calls = 0;
int g_open_option_calls = 0;
int g_open_panel_calls = 0;
}  // namespace

namespace qol::features::core_ui {
void panels_bind_sdk(const contracts::QolHostSdk*, contracts::QolOwnerToken) { ++g_bind_sdk_calls; }
bool panels_register() { ++g_panels_register_calls; return true; }
void panels_unregister() { ++g_panels_unregister_calls; }
bool plugin_enabled() { return false; }
void plugin_set_enabled(bool) {}
const char* settings_more_games_bind_state() { return "waiting"; }
const contracts::PanelCallbacks* settings_panel_callbacks() { return nullptr; }
bool settings_provider_open() { ++g_provider_open_calls; return true; }
bool settings_panels_open(const char*) { ++g_panels_open_calls; return true; }
std::string data_settings_ui_inject() { ++g_inject_calls; return "{}"; }
std::string data_settings_ui_restore() { ++g_restore_calls; return "{}"; }
std::string data_settings_ui_open_option() { ++g_open_option_calls; return "{}"; }
std::string data_settings_ui_open_panel() { ++g_open_panel_calls; return "{}"; }
}  // namespace qol::features::core_ui

namespace {

// feature_control 是可用的最小面：只有 load 期可用性检查，插件不调用任何函数。
QolResult fc_read_catalog(void*, const qol::contracts::QolFeatureCatalogPlugin**, uint32_t*) {
    return qol::contracts::ok();
}
QolResult fc_read_desired(void*, const char*, uint8_t*, uint8_t*) { return qol::contracts::ok(); }
QolResult fc_read_feature(void*, const char*, const char*, uint8_t*) { return qol::contracts::ok(); }
QolResult fc_request_set_feature(void*, const char*, const char*, uint8_t,
                                 qol::contracts::FeatureReceipt*) {
    return qol::contracts::ok();
}
QolResult fc_request_desired(void*, const char*, uint8_t, qol::contracts::FeatureReceipt*) {
    return qol::contracts::ok();
}
QolResult fc_receipt(void*, uint64_t, qol::contracts::FeatureReceipt*) {
    return qol::contracts::ok();
}
QolResult fc_invoke_entry(void*, const char*, const char*) { return qol::contracts::ok(); }

struct RegisteredCommand {
    qol::contracts::HttpMethod method;
    std::string path;
    std::string owner;
    qol::contracts::HttpCommandHandlerFn handler;
};
std::vector<RegisteredCommand> g_commands;
std::string g_unregistered_owner;
int g_unregister_calls = 0;

QolResult register_command(qol::contracts::HttpMethod method, const char* path, const char* owner,
                           qol::contracts::HttpCommandHandlerFn handler, void*) {
    g_commands.push_back({method, path ? path : "", owner ? owner : "", handler});
    return qol::contracts::ok();
}
QolResult unregister_owner(const char* owner) {
    ++g_unregister_calls;
    g_unregistered_owner = owner ? owner : "";
    return qol::contracts::ok();
}

qol::contracts::QolFrameHandler g_frame_handler = nullptr;
uint64_t g_removed_task = 0;
QolResult scheduler_add(int32_t, qol::contracts::QolFrameHandler handler, void*, int32_t, int32_t,
                        uint64_t* out_id) {
    if (out_id == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    g_frame_handler = handler;
    *out_id = 77;
    return qol::contracts::ok();
}
QolResult scheduler_remove(uint64_t id) {
    g_removed_task = id;
    return qol::contracts::ok();
}

const qol::contracts::HttpCommandHandlerFn* find_command(const char* path) {
    for (const RegisteredCommand& command : g_commands)
        if (command.path == path) return &command.handler;
    return nullptr;
}

std::string invoke_command(QolResult (*handler)(const qol::contracts::HttpReq*,
                                                qol::contracts::HttpRes*, void*)) {
    uint8_t output[64]{};
    qol::contracts::HttpRes response{output, sizeof(output), 0, 0};
    const qol::contracts::HttpReq request{qol::contracts::HttpMethod::kPost, "", 0, nullptr, 0,
                                          nullptr, 0, nullptr, 0};
    if (handler == nullptr || !qol::contracts::succeeded(handler(&request, &response, nullptr)) ||
        response.status != 200)
        return {};
    return std::string(reinterpret_cast<const char*>(output), response.body_size);
}

}  // namespace

int main() {
    using namespace qol;
    const contracts::PluginDescriptor& descriptor = features::core_settings::plugin_descriptor();

    // Shape: no resources, no descriptor-declared HTTP queries, exactly one page entry.
    if (descriptor.resources_count != 0 || descriptor.resources != nullptr) return 1;
    if (descriptor.http_query_count != 0 || descriptor.http_queries != nullptr) return 1;
    if (descriptor.feature_count != 1 || descriptor.features == nullptr) return 1;
    const contracts::FeatureDefinition& page = descriptor.features[0];
    if (std::strcmp(page.id, "page") != 0 || page.default_enabled != 1 ||
        page.category != contracts::FeatureCategory::kTools ||
        page.type != contracts::FeatureType::kEntry || page.on_toggle != nullptr ||
        page.on_entry == nullptr || page.name == nullptr || page.description == nullptr)
        return 1;

    contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(contracts::QolHostSdk), contracts::kHostSdkMajor, contracts::kHostSdkMinor};
    sdk.http_dispatch.register_command = &register_command;
    sdk.http_dispatch.unregister_owner = &unregister_owner;
    sdk.scheduler = {&scheduler_add, &scheduler_remove};
    contracts::PluginContext context{};
    context.sdk = &sdk;
    context.owner_token = 42;

    // install is fail-closed without the resident feature_control channel.
    if (descriptor.install == nullptr || descriptor.install(context) == 0) return 1;
    if (!g_commands.empty()) return 1;

    contracts::QolFeatureControlApi feature_control{
        sizeof(contracts::QolFeatureControlApi), nullptr, &fc_read_catalog, &fc_read_desired,
        &fc_read_feature, &fc_request_set_feature, &fc_request_desired, &fc_receipt,
        &fc_invoke_entry, nullptr, nullptr, nullptr};
    sdk.feature_control = &feature_control;

    if (descriptor.install(context) != 0) return 1;
    if (g_bind_sdk_calls != 1 || g_panels_register_calls != 1) return 1;

    // Exactly the four debug settings-ui POST commands, in declaration order.
    const char* const expected_paths[] = {"/api/debug/settings-ui/inject",
                                          "/api/debug/settings-ui/restore",
                                          "/api/debug/settings-ui/open-option",
                                          "/api/debug/settings-ui/open-panel"};
    if (g_commands.size() != 4) return 1;
    for (size_t index = 0; index < g_commands.size(); ++index) {
        const RegisteredCommand& command = g_commands[index];
        if (command.method != contracts::HttpMethod::kPost ||
            command.path != expected_paths[index] || command.owner != "settings" ||
            command.handler == nullptr)
            return 1;
    }

    // Every route queues its UI action; the resident frame task applies it exactly once.
    if (invoke_command(*find_command(expected_paths[0])) != "{\"ok\":true}" ||
        invoke_command(*find_command(expected_paths[1])) != "{\"ok\":true}" ||
        invoke_command(*find_command(expected_paths[2])) != "{\"ok\":true}" ||
        invoke_command(*find_command(expected_paths[3])) != "{\"ok\":true}")
        return 1;
    if (find_command(expected_paths[0]) == nullptr || g_frame_handler == nullptr) return 1;
    g_inject_calls = g_restore_calls = g_open_option_calls = g_open_panel_calls = 0;
    {
        uint8_t output[64]{};
        contracts::HttpRes response{output, sizeof(output), 0, 0};
        const contracts::HttpReq request{contracts::HttpMethod::kPost, expected_paths[0], 0, nullptr,
                                         0, nullptr, 0, nullptr, 0};
        if (!contracts::succeeded((*find_command(expected_paths[0]))(&request, &response, nullptr)))
            return 1;
    }
    if (g_frame_handler(0, nullptr) == 0) return 1;  // resident: applies the queued inject action
    if (g_inject_calls != 1 || g_restore_calls != 0 || g_open_option_calls != 0 ||
        g_open_panel_calls != 0)
        return 1;
    if (g_frame_handler(0, nullptr) == 0) return 1;  // no pending action
    if (g_inject_calls != 1) return 1;

    // The page entry delegates to the core_ui provider/panel stubs.
    if (!contracts::succeeded(page.on_entry(context))) return 1;
    if (g_provider_open_calls != 1 || g_panels_open_calls != 1) return 1;

    // deactivate removes the scheduler task and the self-registered command owner.
    descriptor.deactivate(context);
    if (g_removed_task != 77 || g_unregister_calls != 1 || g_unregistered_owner != "settings" ||
        g_panels_unregister_calls != 1)
        return 1;

    std::printf("core_settings_config_tests: pass\n");
    return 0;
}
