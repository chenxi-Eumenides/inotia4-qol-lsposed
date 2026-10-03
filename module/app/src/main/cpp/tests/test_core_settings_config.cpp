#include "contracts/http.h"
#include "contracts/host_sdk.h"
#include "contracts/plugin.h"
#include "plugins/core.settings/plugin.h"
#include "plugins/core.settings/panels/settings.h"
#include "infra/config.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace qol::features::core_ui {
void panels_bind_sdk(const contracts::QolHostSdk*) {}
bool panels_register() { return true; }
void panels_unregister() {}
bool plugin_enabled() { return false; }
void plugin_set_enabled(bool) {}
const char* settings_more_games_bind_state() { return "waiting"; }
const contracts::PanelCallbacks* settings_panel_callbacks() { return nullptr; }
std::string data_settings_ui_inject() { return "{}"; }
std::string data_settings_ui_restore() { return "{}"; }
std::string data_settings_ui_open_option() { return "{}"; }
std::string data_settings_ui_open_panel() { return "{}"; }
}  // namespace qol::features::core_ui

namespace {

QolResult config_get_bool(const char* key, int32_t fallback, int32_t* out) {
    if (key == nullptr || out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = qol::infra::config_get_bool(key, fallback != 0) ? 1 : 0;
    return qol::contracts::ok();
}

QolResult config_get_i32(const char* key, int32_t fallback, int32_t* out) {
    if (key == nullptr || out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = qol::infra::config_get_int(key, fallback);
    return qol::contracts::ok();
}

QolResult config_get_string(const char* key, const char* fallback, char* out,
                            uint32_t capacity, uint32_t* out_size) {
    if (key == nullptr || fallback == nullptr || out == nullptr || capacity == 0 || out_size == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    const char* value = qol::infra::config_get_string(key, fallback);
    const size_t length = std::strlen(value);
    if (length >= capacity) return qol::contracts::error(kQolErrBlocked, "small", 5);
    std::memcpy(out, value, length + 1);
    *out_size = static_cast<uint32_t>(length);
    return qol::contracts::ok();
}

QolResult register_command(qol::contracts::HttpMethod, const char*, const char*,
                           qol::contracts::HttpCommandHandlerFn, void*) {
    return qol::contracts::ok();
}

QolResult scheduler_add(int32_t, qol::contracts::QolFrameHandler, void*, int32_t, int32_t,
                        uint64_t* out_id) {
    static uint64_t next_id = 1;
    if (out_id == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out_id = next_id++;
    return qol::contracts::ok();
}

QolResult scheduler_remove(uint64_t) { return qol::contracts::ok(); }

}  // namespace

int main() {
    using namespace qol;
    const contracts::PluginDescriptor& descriptor = features::core_settings::plugin_descriptor();
    if (descriptor.resources_count != 0 || descriptor.resources != nullptr) return 1;
    if (descriptor.http_query_count != 1 || descriptor.http_queries == nullptr) return 1;
    const contracts::HttpQueryHandlerDescriptor& query = descriptor.http_queries[0];
    if (query.method != contracts::HttpMethod::kGet ||
        std::strcmp(query.path, "/api/config/list") != 0 ||
        std::strcmp(query.owner, "core.settings") != 0) {
        return 1;
    }

    infra::config_reset_for_test();
    if (!infra::config_set_string("listenAddress", "host\"\\\n") ||
        !infra::config_set_int("listenPort", -42) ||
        !infra::config_set_bool("stackLimitIncrease", true) ||
        !infra::config_set_bool("moveMergeEnabled", false) ||
        !infra::config_set_bool("opEnabled", true) ||
        !infra::config_set_bool("extensionBagEnabled", false) ||
        !infra::config_set_bool("customRecipeEnabled", true) ||
        !infra::config_set_bool("simpleModeEnabled", false) ||
        !infra::config_set_bool("pushAllEnabled", true) ||
        !infra::config_set_bool("autoSellEnabled", false) ||
        !infra::config_set_bool("apiEnabled", true) ||
        !infra::config_set_bool("debugLogEnabled", false)) {
        return 1;
    }

    contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(contracts::QolHostSdk), contracts::kHostSdkMajor, contracts::kHostSdkMinor};
    sdk.config.get_bool = &config_get_bool;
    sdk.config.get_i32 = &config_get_i32;
    sdk.config.get_string = &config_get_string;
    sdk.http_dispatch.register_command = &register_command;
    sdk.scheduler = {&scheduler_add, &scheduler_remove};
    contracts::PluginContext context{};
    context.sdk = &sdk;
    if (descriptor.install == nullptr || descriptor.install(context) != 0) return 1;

    uint8_t output[1024]{};
    contracts::HttpRes response{output, sizeof(output), 0, 0};
    const contracts::HttpReq request{contracts::HttpMethod::kGet,
                                     query.path,
                                     static_cast<uint32_t>(std::strlen(query.path)),
                                     nullptr,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     0};
    const QolResult result = query.handler(&request, &response, query.ctx);
    const std::string body(reinterpret_cast<const char*>(output), response.body_size);
    const std::string expected =
        "{\"listenAddress\":\"host\\\"\\\\\\n\",\"listenPort\":-42,"
        "\"stackLimitIncrease\":true,\"moveMergeEnabled\":false,\"opEnabled\":true,"
        "\"extensionBagEnabled\":false,\"customRecipeEnabled\":true,"
        "\"simpleModeEnabled\":false,\"pushAllEnabled\":true,\"autoSellEnabled\":false,"
        "\"apiEnabled\":true,\"debugLogEnabled\":false}";
    if (!contracts::succeeded(result) || response.status != 200 || body != expected) {
        std::printf("core_settings config response mismatch: %s\n", body.c_str());
        return 1;
    }
    descriptor.deactivate(context);
    std::printf("core_settings_config_tests: pass\n");
    return 0;
}
