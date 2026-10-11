#include "host/features/production.h"
#include "host/features/platform.h"
#include "host/features/http_control.h"
#include "host/owner_identity.h"

#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <type_traits>
#include <vector>

#include "infra/status.h"

using namespace qol::contracts;
using namespace qol::host;
using namespace qol::features::core_settings;

namespace {
int business_calls = 0;
int prepare_calls = 0;
bool g_api_dispatch_enabled = false;
QolResult toggle(PluginContext&, uint8_t) { ++business_calls; return ok(); }
QolResult entry(PluginContext&) { ++business_calls; return ok(); }
constexpr FeatureDefinition definitions[] = {
    {"switch", 0, FeatureCategory::kBasicOptimization, FeatureType::kToggle,
     "switch-name", "switch-description", &toggle, nullptr},
    {"open", 1, FeatureCategory::kTools, FeatureType::kEntry,
     "entry-name", "entry-description", nullptr, &entry, "metadata-action"},
};
constexpr FeatureDefinition core_custom_ui_features[] = {
    {"core.custom_ui.open_debug_components", 1, FeatureCategory::kHidden,
     FeatureType::kEntry, "打开调试面板", "", nullptr, &entry, "打开"},
    {"core.custom_ui.ui_tools", 1, FeatureCategory::kHidden,
     FeatureType::kToggle, "共享界面工具", "", &toggle, nullptr},
    {"core.custom_ui.other_entry", 1, FeatureCategory::kHidden,
     FeatureType::kEntry, "其他调试入口", "", nullptr, &entry, "打开"},
    {"core.custom_ui.tools_entry", 1, FeatureCategory::kTools,
     FeatureType::kEntry, "非隐藏入口", "", nullptr, &entry, "打开"},
    {"core.custom_ui.debug_toggle", 1, FeatureCategory::kHidden,
     FeatureType::kToggle, "调试开关", "", &toggle, nullptr},
};
constexpr FeatureDefinition missing_entry_callback[] = {
    {"core.invalid.debug_entry", 1, FeatureCategory::kHidden,
     FeatureType::kEntry, "无回调入口", "", nullptr, nullptr, "打开"},
};
PluginDescriptor descriptor(const char* id) {
    PluginDescriptor out{};
    out.id = id;
    out.features = definitions;
    out.feature_count = 2;
    return out;
}
const PluginDescriptor alpha_descriptor = descriptor("plugin.alpha");
const PluginDescriptor beta_descriptor = descriptor("plugin.beta");
const PluginDescriptor core_descriptor = descriptor("core.test");
const PluginDescriptor settings_descriptor = descriptor("settings");
PluginDescriptor make_debug_custom_ui_descriptor() {
    auto out = descriptor("core.custom_ui");
    out.features = core_custom_ui_features;
    out.feature_count = sizeof(core_custom_ui_features) / sizeof(core_custom_ui_features[0]);
    return out;
}
const PluginDescriptor core_custom_ui_descriptor = make_debug_custom_ui_descriptor();
PluginDescriptor missing_callback_descriptor() {
    auto out = descriptor("core.invalid_callback");
    out.features = missing_entry_callback;
    out.feature_count = 1;
    return out;
}

QolOwnerToken owner(void* identity, const char* id) {
    assert(owner_register(identity, id));
    QolOwnerToken token = 0;
    uint64_t generation = 0;
    assert(succeeded(owner_identity_api().query(identity, &token, &generation)));
    return token;
}

FeatureReceipt await(const QolFeatureControlApi& api, FeatureReceipt value) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        assert(succeeded(api.receipt(api.context, value.request_id, &value)));
        if (value.persistence != FeaturePersistence::kPending) return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    std::fputs("feature receipt timed out\n", stderr);
    std::abort();
}

void test_abi() {
    static_assert(std::is_standard_layout_v<QolFeatureControlApi> &&
                  std::is_trivial_v<QolFeatureControlApi>);
    static_assert(std::is_standard_layout_v<QolFeatureCatalogItem> &&
                  std::is_trivially_copyable_v<QolFeatureCatalogItem>);
    static_assert(offsetof(QolHostSdk, config_namespace) == 800);
    static_assert(offsetof(QolHostSdk, feature) == 808);
    static_assert(offsetof(QolHostSdk, feature_control) == 816);
    static_assert(static_cast<uint8_t>(FeatureCategory::kGeneral) == 0 &&
                  static_cast<uint8_t>(FeatureCategory::kGameplay) == 1 &&
                  static_cast<uint8_t>(FeatureCategory::kTools) == 2 &&
                  static_cast<uint8_t>(FeatureCategory::kHidden) == 3 &&
                  static_cast<uint8_t>(FeatureCategory::kBasicOptimization) == 4);
    auto feature = production_feature_api();
    auto control = production_feature_control_api();
    QolHostSdk sdk{};
    sdk.abi = {sizeof(QolHostSdk), kHostSdkMajor, kHostSdkMinor};
    sdk.feature = &feature;
    assert(feature_if_available(nullptr) == nullptr);
    assert(feature_control_if_available(nullptr) == nullptr);
    assert(feature_if_available(&sdk) == &feature);
    assert(feature_control_if_available(&sdk) == nullptr); // ordinary SDK
    sdk.feature_control = &control;
    assert(feature_control_if_available(&sdk) == &control);
    sdk.abi.minor = 21;
    assert(feature_if_available(&sdk) == nullptr);
    assert(feature_control_if_available(&sdk) == nullptr);
    assert(!production_feature_bind_management_sdk(sdk));
    sdk.abi.minor = 22;
    sdk.abi.major = 2;
    assert(feature_if_available(&sdk) == nullptr);
    assert(feature_control_if_available(&sdk) == nullptr);
    sdk.abi.major = 1;
    sdk.abi.size = offsetof(QolHostSdk, feature);
    assert(feature_if_available(&sdk) == nullptr);
    sdk.abi.size = offsetof(QolHostSdk, feature) + sizeof(sdk.feature) - 1;
    assert(feature_if_available(&sdk) == nullptr);
    sdk.abi.size = offsetof(QolHostSdk, feature_control);
    assert(feature_if_available(&sdk) == &feature);
    assert(feature_control_if_available(&sdk) == nullptr);
    assert(!production_feature_bind_management_sdk(sdk));
    sdk.abi.size = offsetof(QolHostSdk, feature_control) + sizeof(sdk.feature_control) - 1;
    assert(feature_control_if_available(&sdk) == nullptr);
    assert(!production_feature_bind_management_sdk(sdk));
    sdk.abi.size = sizeof(sdk);
    --feature.struct_size;
    assert(feature_if_available(&sdk) == nullptr);
    ++feature.struct_size;
    feature.invoke_entry = nullptr;
    assert(feature_if_available(&sdk) == nullptr);
    feature = production_feature_api();
    control.struct_size = offsetof(QolFeatureControlApi, invoke_entry) + sizeof(control.invoke_entry) - 1;
    assert(feature_control_if_available(&sdk) == nullptr);
    control.struct_size = sizeof(control);
    control.receipt = nullptr;
    assert(feature_control_if_available(&sdk) == nullptr);
    sdk.feature_control = nullptr;
    assert(production_feature_bind_management_sdk(sdk));
    assert(feature_control_if_available(&sdk) == &production_feature_control_api());
}
}  // namespace

// Focused registry seam: no resource engine or product callbacks are installed.
// Inactive contexts never become business-ready; only admission/queue behavior is tested.
namespace qol::host {
void http_dispatch_set_api_enabled(bool enabled) { g_api_dispatch_enabled = enabled; }
uint32_t plugin_count() { return 5; }
const contracts::PluginDescriptor* find_plugin(const char* id) {
    if (id && std::strcmp(id, "plugin.alpha") == 0) return &alpha_descriptor;
    if (id && std::strcmp(id, "plugin.beta") == 0) return &beta_descriptor;
    if (id && std::strcmp(id, "core.test") == 0) return &core_descriptor;
    if (id && std::strcmp(id, "settings") == 0) return &settings_descriptor;
    if (id && std::strcmp(id, "core.custom_ui") == 0) return &core_custom_ui_descriptor;
    return nullptr;
}
bool feature_registry_prepare(const HostDeps&) { ++prepare_calls; return true; }
void feature_registry_bind_desired_request(FeatureDesiredRequest, void*) {}
uint32_t feature_registry_order(const char**, uint32_t) { return 0; }
bool feature_registry_dependencies_wanted(const char*, FeatureWants, void*) { return true; }
bool feature_registry_dependencies_ready(const char*) { return true; }
bool feature_registry_busy(const char*) { return false; }
bool feature_registry_active(const char*) { return false; }
bool feature_registry_transition(const char*, bool) { return false; }
bool feature_registry_context(const char*, contracts::PluginContext*) { return false; }
void feature_registry_set_wanted(const char*, bool) {}
uint32_t feature_registry_step() { return 0; }
bool feature_registry_owner_ready(const char*) { return true; }
}  // namespace qol::host

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string path = std::string(argv[1]) + "/production-feature-XXXXXX";
    assert(::mkdtemp(path.data()));
    const auto file_path = path + "/config.json";
    struct stat stat_buffer{};
    assert(::stat(file_path.c_str(), &stat_buffer) != 0);
    assert(core_settings_config_service().state() == CoreSettingsConfigServiceState::kUninitialized);
    test_abi();
    assert(&production_feature_controller().api() == &production_feature_api());
    assert(&production_feature_reconciler() == &production_feature_reconciler());
    assert(production_feature_directory().definitions().empty());
    assert(core_settings_config_service().revision() == 0);
    assert(core_settings_config_service().state() == CoreSettingsConfigServiceState::kUninitialized);
    assert(::stat(file_path.c_str(), &stat_buffer) != 0 && prepare_calls == 0);
    const auto& control = production_feature_control_api();
    const QolFeatureCatalogPlugin* catalog = nullptr;
    uint32_t count = 0;
    assert(control.read_catalog(control.context, &catalog, &count).code == kQolErrNotReady);
    assert(!production_feature_bind_frame({}));
    auto missing = alpha_descriptor;
    missing.feature_count = 0;
    assert(!production_feature_register(missing).ok());
    assert(production_feature_directory().definitions().empty());
    assert(production_feature_register(alpha_descriptor).ok());
    assert(production_feature_register(beta_descriptor).ok());
    assert(!production_feature_register(alpha_descriptor).ok());
    assert(production_feature_register_host_owner(platform_config_descriptor()).ok());
    assert(production_feature_register(core_descriptor).ok());
    assert(production_feature_register(settings_descriptor).ok());
    const auto missing_callback = missing_callback_descriptor();
    const size_t definitions_before_invalid = production_feature_directory().definitions().size();
    assert(!production_feature_register(missing_callback).ok());
    assert(production_feature_directory().definitions().size() == definitions_before_invalid);
    assert(production_feature_register(core_custom_ui_descriptor).ok());
    {
        std::ofstream file(file_path);
        file << R"({"module.platform":{"enable":true,"features":{"op.enabled":false,"debug.logging":false},"config":{}}})";
    }
    assert(production_feature_bootstrap(path).ok());
    assert(!production_feature_register(descriptor("late")).ok());
    assert(!production_feature_register_host_owner(platform_config_descriptor()).ok());
    assert(!production_feature_bootstrap(path).ok());
    assert(succeeded(control.read_catalog(control.context, &catalog, &count)) && count == 4);
    assert(std::strcmp(catalog[2].plugin_id, kPlatformConfigOwner) == 0 && catalog[2].host_owned == 1);
    assert(catalog[2].feature_count == 2);
    assert(std::strcmp(catalog[2].features[0].id, "api.enabled") == 0 &&
           std::strcmp(catalog[2].features[0].name, "API服务") == 0);
    assert(std::strcmp(catalog[2].features[1].id, "debug.logging") == 0);
    uint8_t api_enabled = 0;
    assert(succeeded(control.read_feature(control.context, kPlatformConfigOwner, "api.enabled", &api_enabled)) && api_enabled);
    assert(control.read_feature(control.context, kPlatformConfigOwner, "op.enabled", &api_enabled).code == kQolErrNotFound);
    assert(g_api_dispatch_enabled); // Legacy op.enabled=false does not close operation APIs.
    assert(catalog[0].host_owned == 0);
    assert(std::strcmp(catalog[0].plugin_id, "plugin.alpha") == 0);
    assert(catalog[0].feature_count == 2);
    assert(std::strcmp(catalog[0].features[0].id, "switch") == 0);
    assert(production_feature_directory().definitions()[0].features[0].on_toggle == &toggle);
    assert(catalog[0].features[1].type == FeatureType::kEntry);
    assert(catalog[0].features[1].default_enabled == 1);
    assert(production_feature_directory().definitions()[0].features[1].on_entry == &entry);
    assert(std::strcmp(catalog[0].features[1].description, "entry-description") == 0);
    assert(std::strcmp(production_feature_directory().definitions()[0].features[1].action_label,
                       "metadata-action") == 0);
    assert(std::strcmp(production_feature_controller().catalog()[0].features[1].action_label,
                       "metadata-action") == 0);
    assert(std::strcmp(catalog[0].features[1].action_label, "metadata-action") == 0);
    assert(production_feature_directory().definitions()[0].features[0].action_label == nullptr);
    assert(catalog[0].features[0].action_label == nullptr);
    // Core plugins stay registered for lifecycle/config management. Only the exact hidden
    // debug entry from core.custom_ui is projected; its other valid features stay excluded.
    assert(production_feature_directory().definitions().size() == 6);
    uint8_t core_enabled = 0;
    assert(succeeded(control.read_feature(control.context, "core.test", "open", &core_enabled)) && core_enabled == 1);
    assert(succeeded(control.read_feature(control.context, "settings", "open", &core_enabled)) && core_enabled == 1);
    assert(catalog[3].host_owned == 0 &&
           std::strcmp(catalog[3].plugin_id, "core.custom_ui") == 0);
    assert(catalog[3].feature_count == 1);
    const auto& debug_entry = catalog[3].features[0];
    assert(std::strcmp(debug_entry.id, "core.custom_ui.open_debug_components") == 0);
    assert(debug_entry.type == FeatureType::kEntry &&
           debug_entry.category == FeatureCategory::kHidden && debug_entry.default_enabled == 1);
    assert(std::strcmp(debug_entry.action_label, "打开") == 0 &&
           std::strcmp(debug_entry.description, "") == 0);
    const auto& registered_custom_ui = production_feature_directory().definitions()[5];
    assert(std::strcmp(registered_custom_ui.plugin_id.c_str(), "core.custom_ui") == 0);
    assert(registered_custom_ui.features[0].on_entry == &entry);
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(catalog[i].plugin_id, "core.custom_ui") == 0) continue;
        assert(std::strncmp(catalog[i].plugin_id, "core.", 5) != 0);
        assert(std::strcmp(catalog[i].plugin_id, "settings") != 0);
    }

    int alpha_identity = 0, manager_identity = 0;
    const auto alpha = owner(&alpha_identity, "plugin.alpha");
    const auto manager = owner(&manager_identity, "plugin.manager");
    const auto& api = production_feature_api();
    char buffer[8192]{};
    uint32_t size = 0;
    uint8_t enabled = 9, any = 9;
    FeatureReceipt receipt{};
    assert(succeeded(api.read_namespace(api.context, alpha, "plugin.alpha", buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size).find("\"enable\"") == std::string::npos);
    assert(api.read_feature(api.context, alpha, "plugin.beta", "open", &enabled).code == kQolErrBlocked);
    assert(api.request_set_feature(api.context, alpha, "plugin.beta", "open", 0, &receipt).code == kQolErrBlocked);
    assert(api.request_desired(api.context, alpha, "plugin.alpha", 0, &receipt).code == kQolErrBlocked);
    assert(api.read_desired(api.context, alpha, "plugin.alpha", &enabled, &any).code == kQolErrBlocked);
    assert(api.read_catalog(api.context, manager, buffer, sizeof(buffer), &size).code == kQolErrBlocked);
    assert(succeeded(production_feature_controller().grant_admin(manager)));
    assert(succeeded(api.read_catalog(api.context, manager, buffer, sizeof(buffer), &size)));
    const std::string expected_plugin = R"([{"id":"switch","name":"switch-name","description":"switch-description","type":"toggle","category":4,"default":false},{"id":"open","name":"entry-name","description":"entry-description","type":"entry","category":2,"default":true}])";
    const std::string catalog_json(buffer, size);
    assert(catalog_json.rfind("{\"plugin.alpha\":" + expected_plugin + ",\"plugin.beta\":" + expected_plugin + ",\"module.platform\":[", 0) == 0);
    assert(catalog_json.find("\"api.enabled\"") != std::string::npos &&
           catalog_json.find("\"op.enabled\"") == std::string::npos &&
           catalog_json.find("\"debug.logging\"") != std::string::npos);

    assert(api.invoke_entry(api.context, alpha, "plugin.alpha", "open").code == kQolErrNotReady);
    assert(production_feature_bind_frame({}) && prepare_calls == 1);
    assert(succeeded(control.invoke_entry(control.context, "plugin.alpha", "open")));
    assert(business_calls == 0); // admission never executes on this caller
    production_feature_frame_step();
    assert(business_calls == 0); // inactive registry context guard
    owner_suspend(&alpha_identity);
    owner_suspend(&manager_identity);
    assert(api.read_feature(api.context, alpha, "plugin.alpha", "open", &enabled).code != kQolOk);
    assert(api.read_catalog(api.context, manager, buffer, sizeof(buffer), &size).code != kQolOk);
    assert(succeeded(control.read_catalog(control.context, &catalog, &count)) && count == 4);
    assert(succeeded(control.read_desired(control.context, "plugin.alpha", &enabled, &any)) && enabled && any);
    assert(succeeded(control.request_set_feature(control.context, "plugin.alpha", "open", 0, &receipt)));
    const auto host_receipt = receipt.request_id;
    receipt = await(control, receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.read_desired(control.context, "plugin.alpha", &enabled, &any)) && !enabled && !any);
    assert(control.invoke_entry(control.context, "plugin.alpha", "open").code == kQolErrBlocked);
    assert(succeeded(control.request_desired(control.context, "plugin.alpha", 0, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.request_set_feature(control.context, "plugin.alpha", "open", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.read_desired(control.context, "plugin.alpha", &enabled, &any)) && enabled && any);
    assert(succeeded(control.request_desired(control.context, "plugin.alpha", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.receipt(control.context, host_receipt, &receipt)));
    assert(owner_resume(&alpha_identity));
    QolOwnerToken refreshed = 0;
    uint64_t generation = 0;
    assert(succeeded(owner_identity_api().query(&alpha_identity, &refreshed, &generation)));
    assert(refreshed != alpha);
    assert(api.receipt(api.context, refreshed, host_receipt, &receipt).code == kQolErrBlocked);
    assert(succeeded(api.request_set_config(api.context, refreshed, "plugin.alpha", "parameter", "3", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(api.read_config(api.context, refreshed, "plugin.alpha", "parameter", buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size) == "3");
    assert(business_calls == 0);

    // Host-owned platform owner: resident, actual always enabled, desired toggles rejected.
    int32_t actual_state = -1;
    uint8_t actual_enabled = 9;
    assert(succeeded(control.read_actual(control.context, kPlatformConfigOwner, &actual_state, &actual_enabled)));
    assert(actual_state == static_cast<int32_t>(qol::infra::FeatureState::kEnabled) && actual_enabled == 1);
    assert(control.read_actual(control.context, kPlatformConfigOwner, &actual_state, &actual_enabled).code == kQolOk);
    assert(control.request_desired(control.context, kPlatformConfigOwner, 0, &receipt).code == kQolErrUnsupported);

    // API service is the sole API gate; its value also reaches the native operation route gate.
    assert(succeeded(control.read_desired(control.context, kPlatformConfigOwner, &enabled, &any)) && enabled && any);
    assert(succeeded(control.request_set_feature(control.context, kPlatformConfigOwner, "api.enabled", 0, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.read_desired(control.context, kPlatformConfigOwner, &enabled, &any)) && !enabled && !any);
    assert(!g_api_dispatch_enabled);
    assert(succeeded(control.read_actual(control.context, kPlatformConfigOwner, &actual_state, &actual_enabled)) &&
           actual_state == static_cast<int32_t>(qol::infra::FeatureState::kEnabled) && actual_enabled == 1);
    assert(control.request_set_feature(control.context, kPlatformConfigOwner, "op.enabled", 1, &receipt).code == kQolErrNotFound);
    assert(succeeded(control.read_desired(control.context, kPlatformConfigOwner, &enabled, &any)) && !enabled && !any);
    assert(succeeded(control.request_set_feature(control.context, kPlatformConfigOwner, "api.enabled", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.read_desired(control.context, kPlatformConfigOwner, &enabled, &any)) && enabled && any);
    assert(g_api_dispatch_enabled); // API=true overrides the preserved legacy op.enabled=false.

    // Platform config validation is enforced by the single worker writer.
    const char* bad_port = "{\"kind\":\"number\"}";
    assert(control.request_set_config(control.context, kPlatformConfigOwner, "listen_port", bad_port, std::strlen(bad_port), &receipt).code == kQolErrInvalidArgument);
    assert(control.request_set_config(control.context, kPlatformConfigOwner, "listen_address", "127", 3, &receipt).code == kQolErrInvalidArgument); // number where string required
    assert(succeeded(control.request_set_config(control.context, kPlatformConfigOwner, "listen_port", "9000", 4, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.read_config(control.context, kPlatformConfigOwner, "listen_port", buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size) == "9000");
    const auto platform_state = feature_platform_state();
    assert(platform_state.listen_port == 9000 && platform_state.api_enabled);

    // Bind failure on the configured port rewrites listen_port to the default through the
    // feature store and emits a revision effect; repeating on 8088 is rejected (no loop).
    assert(platform_state.revision == qol::host::core_settings_config_service().revision());
    assert(qol::host::core_settings_config_service().report_bind_failure(platform_state.revision, 9000));
    const auto fallback_state = feature_platform_state();
    assert(fallback_state.listen_port == 8088 && fallback_state.revision > platform_state.revision);
    assert(!qol::host::core_settings_config_service().report_bind_failure(platform_state.revision, 9000));
    assert(!qol::host::core_settings_config_service().report_bind_failure(fallback_state.revision, 8088));

    // Host-owned HTTP control routes are resident and settings-independent.
    auto invoke = [](HttpMethod method, const std::string& path, const std::string& body_text) {
        thread_local std::vector<uint8_t> response(65536, 0);
        std::fill(response.begin(), response.end(), 0);
        const auto* body = body_text.empty() ? nullptr : reinterpret_cast<const uint8_t*>(body_text.data());
        HttpReq request{method, path.c_str(), static_cast<uint32_t>(path.size()), body,
                                   static_cast<uint32_t>(body_text.size()), nullptr, 0};
        HttpRes out{response.data(), static_cast<uint32_t>(response.size()), 0, 0};
        assert(feature_control_http_route(request));
        (void)feature_control_http_dispatch(request, out);
        return std::make_pair(out.status, std::string(reinterpret_cast<const char*>(response.data()), out.body_size));
    };
    {
        const auto list = invoke(HttpMethod::kGet, "/api/config/list", "");
        assert(list.first == 200 && list.second.find("\"enable\"") != std::string::npos && list.second.find("plugin.alpha") != std::string::npos);
        assert(list.second.find("\"op.enabled\":false") != std::string::npos);
        // Projection object key order matches the on-disk schema: enable, features, config.
        const auto alpha_object = list.second.find("\"plugin.alpha\":{");
        assert(alpha_object != std::string::npos);
        assert(list.second.compare(alpha_object + std::strlen("\"plugin.alpha\":{"),
                                   std::strlen("\"enable\":"), "\"enable\":") == 0);
        const auto set_feature = invoke(HttpMethod::kPost, "/api/config/set",
            "{\"plugin_id\":\"plugin.alpha\",\"field\":\"feature\",\"key\":\"switch\",\"value\":false}");
        assert(set_feature.first == 200 && set_feature.second.find("\"persistence\":\"pending\"") != std::string::npos);
        const auto lifecycle = invoke(HttpMethod::kPost, "/api/op/features/lifecycle",
            "{\"id\":\"plugin.alpha\",\"action\":\"activate\"}");
        assert(lifecycle.first == 200 && lifecycle.second.find("\"admission\":\"admitted\"") != std::string::npos);
        const auto unknown_field = invoke(HttpMethod::kPost, "/api/config/set",
            "{\"plugin_id\":\"plugin.alpha\",\"field\":\"enable\",\"key\":\"x\",\"value\":true}");
        assert(unknown_field.first == 400); // no generic arbitrary enable field
        const auto bad = invoke(HttpMethod::kPost, "/api/config/set", "{\"plugin_id\":\"plugin.alpha\"}");
        assert(bad.first == 400);
        const auto ordinary = invoke(HttpMethod::kPost, "/api/config/set",
            "{\"plugin_id\":\"plugin.alpha\",\"field\":\"config\",\"key\":\"parameter\",\"value\":42}");
        assert(ordinary.first == 200);
    }
    assert(!feature_control_http_route(HttpReq{HttpMethod::kGet, "/api/item/bag/0", 14, nullptr, 0, nullptr, 0}));
    std::ifstream persisted(file_path);
    const std::string bytes{std::istreambuf_iterator<char>(persisted), {}};
    assert(bytes.find("switch-name") == std::string::npos);
    production_feature_controller().shutdown();
    std::puts("production_feature_facade: PASS (ABI/prefix, explicit metadata, no constructor IO, resident token-free control, owner restrictions, guarded entry admission)");
}
