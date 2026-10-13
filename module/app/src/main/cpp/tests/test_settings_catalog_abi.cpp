#include "contracts/plugin.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

using namespace qol::contracts;
static_assert(kSettingsCatalogMajor == 1 && kSettingsCatalogMinor == 3);
static_assert(kHostSdkMajor == 1 && kHostSdkMinor == 26);
static_assert(kHttpRegistryVersion.major == 1 && kHttpRegistryVersion.minor == 1);
static_assert(kHttpDispatchVersion.major == 1 && kHttpDispatchVersion.minor == 1);

// Frozen v1.15 prefix; no embedded function table may grow to add settings contributions.
struct HostSdkV15 {
    QolAbiHeader abi;
    QolLogApi log;
    QolConfigApi config;
    QolSchedulerApi scheduler;
    QolCommands commands;
    QolEventsApi events;
    QolCapabilitiesApi capabilities;
    QolStoreApi store;
    QolPanelsApi panels;
    QolResultApi result;
    QolHooksApi hooks;
    QolTextScopeApi text_scope;
    const QolUiAdapter* ui;
    QolDescButtonApi desc_button;
    QolInFlightApi in_flight;
    QolHttpRegistryApi http;
    QolHttpDispatchApi http_dispatch;
    QolTicketResultApi ticket_result;
    QolLifecycleParticipantRegisterFn lifecycle_participant_register;
    QolItemHandleApi item_handle;
    QolSnapshotHandle (*snapshot_create)(uint32_t);
    void (*snapshot_destroy)(QolSnapshotHandle);
    bool (*snapshot_publish)(QolSnapshotHandle, const void*, uint32_t);
    bool (*snapshot_acquire)(QolSnapshotHandle, const void**, uint32_t*, QolSnapshotToken*);
    void (*snapshot_release)(QolSnapshotToken);
};

#define PREFIX(field) static_assert(offsetof(QolHostSdk, field) == offsetof(HostSdkV15, field))
PREFIX(abi); PREFIX(log); PREFIX(config); PREFIX(scheduler); PREFIX(commands); PREFIX(events);
PREFIX(capabilities); PREFIX(store); PREFIX(panels); PREFIX(result); PREFIX(hooks); PREFIX(text_scope);
PREFIX(ui); PREFIX(desc_button); PREFIX(in_flight); PREFIX(http); PREFIX(http_dispatch);
PREFIX(ticket_result); PREFIX(lifecycle_participant_register); PREFIX(item_handle);
PREFIX(snapshot_create); PREFIX(snapshot_destroy); PREFIX(snapshot_publish); PREFIX(snapshot_acquire);
PREFIX(snapshot_release);
#undef PREFIX
static_assert(sizeof(QolPanelsApi) == 7 * sizeof(void (*)()));
static_assert(sizeof(PanelItemSpec) == 7 * sizeof(void*));
static_assert(sizeof(PanelProviderSpec) == 7 * sizeof(void*));
static_assert(offsetof(QolHostSdk, settings_catalog) == sizeof(HostSdkV15));
static_assert(sizeof(void*) == 8, "supported Android/host targets use the 64-bit ABI");
static_assert(sizeof(HostSdkV15) == 744);
static_assert(offsetof(QolHostSdk, panels) == 248);
static_assert(offsetof(QolHostSdk, snapshot_release) == 736);
// minor 16 尾（settings_catalog）保持 744/752；minor 17 尾（settings_config）、
// minor 18 尾（module_version）及后续 owner/operation/route/runtime/security/module-health 尾只追加指针。
static_assert(offsetof(QolHostSdk, settings_config) == 752);
static_assert(offsetof(QolHostSdk, module_version) == 760);
static_assert(offsetof(QolHostSdk, owner_verify_token) == 768);
static_assert(offsetof(QolHostSdk, owner_store) == 776);
static_assert(offsetof(QolHostSdk, slot_operation) == 784);
static_assert(offsetof(QolHostSdk, save_route) == 792);
static_assert(offsetof(QolHostSdk, config_namespace) == 800);
static_assert(offsetof(QolHostSdk, feature) == 808);
static_assert(offsetof(QolHostSdk, feature_control) == 816);
static_assert(offsetof(QolHostSdk, runtime_effects) == 824);
static_assert(offsetof(QolHostSdk, security) == 832);
static_assert(offsetof(QolHostSdk, module_health) == 840);
static_assert(sizeof(QolHostSdk) == 848);
static_assert(std::is_standard_layout_v<FeatureDefinition> &&
              std::is_trivially_copyable_v<FeatureDefinition>);
constexpr FeatureDefinition kDefaultPriorityFeature{};
static_assert(kDefaultPriorityFeature.display_priority == 0);
static_assert(kMinFeatureDisplayPriority == -10 && kMaxFeatureDisplayPriority == 10);
static_assert(offsetof(FeatureDefinition, action_label) == 48);
static_assert(offsetof(FeatureDefinition, display_priority) == 56);
static_assert(sizeof(FeatureDefinition) == 64);
static_assert(std::is_standard_layout_v<FeatureCategoryDefinition> &&
              std::is_trivially_copyable_v<FeatureCategoryDefinition>);
static_assert(offsetof(FeatureCategoryDefinition, category) == 0);
static_assert(offsetof(FeatureCategoryDefinition, name) == 8);
static_assert(offsetof(FeatureCategoryDefinition, display_priority) == 16);
static_assert(sizeof(FeatureCategoryDefinition) == 24);
static_assert(std::is_standard_layout_v<QolFeatureCatalogItem> &&
              std::is_trivially_copyable_v<QolFeatureCatalogItem>);
constexpr QolFeatureCatalogItem kDefaultPriorityCatalogItem{};
static_assert(kDefaultPriorityCatalogItem.display_priority == 0);
static_assert(offsetof(QolFeatureCatalogItem, display_priority) == 40);
static_assert(sizeof(QolFeatureCatalogItem) == 48);
static_assert(offsetof(QolFeatureControlApi, read_categories) == 96);
static_assert(sizeof(QolFeatureControlApi) == 104);
using ExpectedReadCategoriesFn =
    QolResult (*)(void*, const FeatureCategoryDefinition**, uint32_t*);
static_assert(std::is_same_v<decltype(QolFeatureControlApi::read_categories),
                             ExpectedReadCategoriesFn>);
static_assert(std::is_standard_layout_v<QolPluginRuntimeEffect>);
static_assert(std::is_trivial_v<QolPluginRuntimeEffect>);
static_assert(offsetof(QolPluginRuntimeEffect, struct_size) == 0);
static_assert(offsetof(QolPluginRuntimeEffect, event_revision) == 8);
static_assert(offsetof(QolPluginRuntimeEffect, config_revision) == 16);
static_assert(offsetof(QolPluginRuntimeEffect, plugin_id) == 24);
static_assert(offsetof(QolPluginRuntimeEffect, actual_enabled) == 32);
static_assert(offsetof(QolPluginRuntimeEffect, config_json) == 40);
static_assert(offsetof(QolPluginRuntimeEffect, config_json_size) == 48);
static_assert(offsetof(QolPluginRuntimeEffect, desired_enabled) == 52);
static_assert(sizeof(QolPluginRuntimeEffect) == 56);
using ExpectedRuntimeSubscribeFn = QolResult (*)(void*, QolOwnerToken,
                                                  QolPluginRuntimeObserver, void*, uint64_t*);
using ExpectedRuntimeUnsubscribeFn = QolResult (*)(void*, QolOwnerToken, uint64_t);
using ExpectedOpEnabledFn = QolResult (*)(void*, uint8_t*);
static_assert(std::is_same_v<decltype(QolRuntimeEffectsApi::subscribe_self),
                             ExpectedRuntimeSubscribeFn>);
static_assert(std::is_same_v<decltype(QolRuntimeEffectsApi::unsubscribe),
                             ExpectedRuntimeUnsubscribeFn>);
static_assert(std::is_same_v<decltype(QolSecurityApi::op_enabled), ExpectedOpEnabledFn>);
static_assert(offsetof(QolRuntimeEffectsApi, struct_size) == 0);
static_assert(offsetof(QolRuntimeEffectsApi, context) == 8);
static_assert(offsetof(QolRuntimeEffectsApi, subscribe_self) == 16);
static_assert(offsetof(QolRuntimeEffectsApi, unsubscribe) == 24);
static_assert(sizeof(QolRuntimeEffectsApi) == 32);
static_assert(offsetof(QolSecurityApi, struct_size) == 0);
static_assert(offsetof(QolSecurityApi, context) == 8);
static_assert(offsetof(QolSecurityApi, op_enabled) == 16);
static_assert(sizeof(QolSecurityApi) == 24);
static_assert(std::is_standard_layout_v<QolModuleHealthSnapshot>);
static_assert(std::is_trivial_v<QolModuleHealthSnapshot>);
static_assert(offsetof(QolModuleHealthSnapshot, struct_size) == 0);
static_assert(offsetof(QolModuleHealthSnapshot, revision) == 8);
static_assert(offsetof(QolModuleHealthSnapshot, known_mask) == 16);
static_assert(offsetof(QolModuleHealthSnapshot, failed_mask) == 20);
static_assert(offsetof(QolModuleHealthSnapshot, main_menu_seen) == 24);
static_assert(sizeof(QolModuleHealthSnapshot) == 32);
using ExpectedModuleHealthReadFn = QolResult (*)(void*, QolModuleHealthSnapshot*);
static_assert(std::is_same_v<decltype(QolHostModuleHealthFactsApi::read), ExpectedModuleHealthReadFn>);
static_assert(offsetof(QolHostModuleHealthFactsApi, struct_size) == 0);
static_assert(offsetof(QolHostModuleHealthFactsApi, context) == 8);
static_assert(offsetof(QolHostModuleHealthFactsApi, read) == 16);
static_assert(sizeof(QolHostModuleHealthFactsApi) == 24);
static_assert(static_cast<uint32_t>(QolModuleHealthBit::kNative) == (1U << 0));
static_assert(static_cast<uint32_t>(QolModuleHealthBit::kFrameHooks) == (1U << 1));
static_assert(static_cast<uint32_t>(QolModuleHealthBit::kModuleInitialization) == (1U << 2));
static_assert(static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage) == (1U << 3));
static_assert(kQolModuleHealthAllMask == 0x0FU);
static_assert(std::is_standard_layout_v<HttpEndpointDescriptor>);
static_assert(std::is_trivial_v<HttpEndpointDescriptor>);
static_assert(std::is_same_v<decltype(HttpEndpointDescriptor::query_handler),
                             HttpQueryHandlerFn>);
static_assert(std::is_same_v<decltype(HttpEndpointDescriptor::command_handler),
                             HttpCommandHandlerFn>);
static_assert(std::is_same_v<PluginConfigValidateFn,
                             QolResult (*)(const char*, const char*, uint32_t)>);
static_assert(offsetof(HttpEndpointDescriptor, method) == 0);
static_assert(offsetof(HttpEndpointDescriptor, path) == 8);
static_assert(offsetof(HttpEndpointDescriptor, route_owner) == 16);
static_assert(offsetof(HttpEndpointDescriptor, kind) == 24);
static_assert(offsetof(HttpEndpointDescriptor, op_required) == 25);
static_assert(offsetof(HttpEndpointDescriptor, lease_required) == 26);
static_assert(offsetof(HttpEndpointDescriptor, query_handler) == 32);
static_assert(offsetof(HttpEndpointDescriptor, command_handler) == 40);
static_assert(offsetof(HttpEndpointDescriptor, ctx) == 48);
static_assert(sizeof(HttpEndpointDescriptor) == 56);
static_assert(static_cast<uint8_t>(HttpHandlerKind::kQuery) == 0);
static_assert(static_cast<uint8_t>(HttpHandlerKind::kCommand) == 1);
static_assert(std::is_standard_layout_v<HttpRouteMetadataDescriptor>);
static_assert(std::is_trivial_v<HttpRouteMetadataDescriptor>);
static_assert(offsetof(HttpRouteMetadataDescriptor, method) == 0);
static_assert(offsetof(HttpRouteMetadataDescriptor, path) == 8);
static_assert(offsetof(HttpRouteMetadataDescriptor, route_owner) == 16);
static_assert(offsetof(HttpRouteMetadataDescriptor, op_required) == 24);
static_assert(offsetof(HttpRouteMetadataDescriptor, lease_required) == 25);
static_assert(sizeof(HttpRouteMetadataDescriptor) == 32);
static_assert(std::is_same_v<decltype(PluginDescriptor::http_endpoints),
                             const HttpEndpointDescriptor*>);
static_assert(std::is_same_v<decltype(PluginDescriptor::http_route_metadata),
                             const HttpRouteMetadataDescriptor*>);
static_assert(std::is_same_v<decltype(PluginDescriptor::http_route_metadata_count), uint32_t>);
static_assert(std::is_same_v<decltype(PluginDescriptor::validate_config),
                             PluginConfigValidateFn>);
static_assert(std::is_aggregate_v<PluginDescriptor>);
static_assert(offsetof(PluginDescriptor, http_queries) == 160);
static_assert(offsetof(PluginDescriptor, http_query_count) == 168);
static_assert(offsetof(PluginDescriptor, features) == 176);
static_assert(offsetof(PluginDescriptor, feature_count) == 184);
static_assert(offsetof(PluginDescriptor, http_endpoints) == 192);
static_assert(offsetof(PluginDescriptor, http_endpoint_count) == 200);
static_assert(offsetof(PluginDescriptor, validate_config) == 208);
static_assert(offsetof(PluginDescriptor, http_route_metadata) == 216);
static_assert(offsetof(PluginDescriptor, http_route_metadata_count) == 224);
static_assert(sizeof(PluginDescriptor) == 232);
static_assert(offsetof(QolConfigNamespaceApi, read_json) == 8);
static_assert(offsetof(QolConfigNamespaceApi, replace_json) == 16);
static_assert(offsetof(QolConfigNamespaceApi, clear) == 24);
static_assert(offsetof(QolConfigNamespaceApi, update_value) == 32);
static_assert(offsetof(QolConfigNamespaceApi, read_registered) == 40);
static_assert(offsetof(QolConfigNamespaceApi, update_http_request) == 48);
static_assert(sizeof(QolConfigNamespaceApi) == 56);
static_assert(std::is_standard_layout_v<QolSettingsCatalogApi>);
static_assert(offsetof(QolSettingsCatalogApi, query_item_availability) == 48);
static_assert(offsetof(QolSettingsCatalogApi, register_items) == 56);
static_assert(offsetof(QolSettingsCatalogApi, register_category_metadata) == 64);
static_assert(sizeof(QolSettingsCatalogApi) == 72);
static_assert(std::is_trivial_v<CatalogView>);
static_assert(std::is_standard_layout_v<QolSettingsConfigApi>);
static_assert(std::is_trivial_v<SettingsConfigRequestStatus>);

QolResult register_category(const SettingsCategorySpec*, const SettingsItemSpec*, uint32_t) { return ok(); }
QolResult unregister_owner(const char*) { return ok(); }
QolResult acquire(const CatalogView**, SettingsCatalogToken*) { return ok(); }
QolResult release(SettingsCatalogToken) { return ok(); }
QolResult invoke(const char*, uint64_t) { return ok(); }
QolResult query_availability(const char*, uint64_t, SettingsItemAvailability*) { return ok(); }
QolResult register_items(const char*, const char*, const SettingsItemSpec*, uint32_t) { return ok(); }
QolResult register_category_metadata(const SettingsCategorySpec*) { return ok(); }
const char* test_module_version() { return "0.7.13"; }
QolResult config_read_bool(const char*, int32_t*) { return ok(); }
QolResult config_request_set_bool(const char*, int32_t, uint64_t*) { return ok(); }
QolResult config_query_request(uint64_t, SettingsConfigRequestStatus*) { return ok(); }
QolResult module_health_read_stub(void*, QolModuleHealthSnapshot*) { return ok(); }

int main() {
    constexpr uint32_t old_catalog_size = offsetof(QolSettingsCatalogApi, query_item_availability);
    QolSettingsCatalogApi catalog{old_catalog_size, 1, 0, &register_category,
                                  &unregister_owner, &acquire, &release, &invoke, nullptr};
    QolSettingsConfigApi config_api{sizeof(QolSettingsConfigApi), kSettingsConfigMajor,
                                    kSettingsConfigMinor, &config_read_bool, &config_request_set_bool,
                                    &config_query_request};
    QolHostSdk sdk{};
    sdk.abi = {sizeof(QolHostSdk), 1, 16};
    sdk.settings_catalog = &catalog;
    assert(settings_catalog_if_available(&sdk) == &catalog);
    assert(settings_catalog_availability_if_available(&sdk) == nullptr);
    catalog.size = sizeof(QolSettingsCatalogApi);
    catalog.minor = 1;
    catalog.query_item_availability = &query_availability;
    assert(settings_catalog_availability_if_available(&sdk) == &catalog);
    assert(settings_catalog_join_if_available(&sdk) == nullptr);  // minor 1 cannot consume tail
    catalog.minor = 2;
    catalog.register_items = &register_items;
    catalog.size = offsetof(QolSettingsCatalogApi, register_category_metadata);
    assert(settings_catalog_join_if_available(&sdk) == &catalog);
    assert(settings_catalog_metadata_if_available(&sdk) == nullptr);
    catalog.size = offsetof(QolSettingsCatalogApi, register_items);
    assert(settings_catalog_join_if_available(&sdk) == nullptr);
    catalog.size = sizeof(QolSettingsCatalogApi);
    catalog.minor = 3;
    catalog.register_category_metadata = &register_category_metadata;
    assert(settings_catalog_metadata_if_available(&sdk) == &catalog);
    catalog.size = offsetof(QolSettingsCatalogApi, register_category_metadata);
    assert(settings_catalog_metadata_if_available(&sdk) == nullptr);
    catalog.size = sizeof(QolSettingsCatalogApi);
    catalog.minor = 0;
    catalog.size = old_catalog_size;
    assert(settings_catalog_if_available(nullptr) == nullptr);
    sdk.abi.minor = 15;
    assert(settings_catalog_if_available(&sdk) == nullptr);
    sdk.abi.minor = 16;
    sdk.abi.size = sizeof(HostSdkV15);
    assert(settings_catalog_if_available(&sdk) == nullptr);
    // size 截断必须恰好覆盖 settings_catalog 指针本身（minor 17 尾加入后 sizeof-1 已不再截断它）。
    sdk.abi.size = offsetof(QolHostSdk, settings_catalog);
    assert(settings_catalog_if_available(&sdk) == nullptr);
    sdk.abi.size = sizeof(QolHostSdk);
    sdk.abi.major = 2;
    assert(settings_catalog_if_available(&sdk) == nullptr);
    sdk.abi.major = 1;
    catalog.major = 2;
    assert(settings_catalog_if_available(&sdk) == nullptr);
    catalog.major = 1;
    catalog.size = old_catalog_size - 1;
    assert(settings_catalog_if_available(&sdk) == nullptr);
    catalog.size = old_catalog_size;
    catalog.invoke_action = nullptr;
    assert(settings_catalog_if_available(&sdk) == nullptr);
    catalog.invoke_action = &invoke;
    sdk.settings_catalog = nullptr;
    assert(settings_catalog_if_available(&sdk) == nullptr);
    // minor 17 尾：旧 minor / 缺尾 / 空指针 / 半截接口都必须协商失败。
    sdk.settings_catalog = &catalog;
    sdk.settings_config = &config_api;
    assert(settings_config_if_available(&sdk) == nullptr);  // minor 16 < 17
    sdk.abi.minor = 17;
    assert(settings_config_if_available(&sdk) == &config_api);
    sdk.abi.size = offsetof(QolHostSdk, settings_config);
    assert(settings_config_if_available(&sdk) == nullptr);
    sdk.abi.size = sizeof(QolHostSdk);
    sdk.settings_config = nullptr;
    assert(settings_config_if_available(&sdk) == nullptr);
    sdk.settings_config = &config_api;
    config_api.size = sizeof(QolSettingsConfigApi) - 1;
    assert(settings_config_if_available(&sdk) == &config_api);
    assert(settings_config_owner_if_available(&sdk) == nullptr);
    config_api.size = sizeof(QolSettingsConfigApi);
    config_api.major = kSettingsConfigMajor + 1;  // minor 0 是合法首版；只拒非法 major
    assert(settings_config_if_available(&sdk) == nullptr);
    config_api.major = kSettingsConfigMajor;
    config_api.request_set_bool = nullptr;
    assert(settings_config_if_available(&sdk) == nullptr);
    config_api.request_set_bool = &config_request_set_bool;
    // minor 18 尾：模块身份版本访问；旧 minor / 缺尾 / 空指针都必须协商失败。
    sdk.module_version = &test_module_version;
    sdk.abi.minor = 17;
    assert(host_sdk_module_version(&sdk) == nullptr);
    sdk.abi.minor = 18;
    assert(host_sdk_module_version(&sdk) != nullptr);
    assert(std::strcmp(host_sdk_module_version(&sdk), "0.7.13") == 0);
    sdk.abi.size = offsetof(QolHostSdk, module_version);
    assert(host_sdk_module_version(&sdk) == nullptr);
    sdk.abi.size = sizeof(QolHostSdk);
    sdk.module_version = nullptr;
    assert(host_sdk_module_version(&sdk) == nullptr);
    sdk.module_version = &test_module_version;
    assert(host_sdk_module_version(nullptr) == nullptr);
    QolHostModuleHealthFactsApi module_health_api{
        sizeof(QolHostModuleHealthFactsApi), nullptr, &module_health_read_stub};
    QolHostSdk health_sdk{};
    health_sdk.abi = {sizeof(QolHostSdk), kHostSdkMajor, kHostSdkMinor};
    health_sdk.module_health = &module_health_api;
    assert(module_health_if_available(&health_sdk) == &module_health_api);
    assert(module_health_if_available(nullptr) == nullptr);
    health_sdk.abi.minor = 24;
    assert(module_health_if_available(&health_sdk) == nullptr);
    health_sdk.abi.minor = 25;
    health_sdk.abi.size = offsetof(QolHostSdk, module_health);
    assert(module_health_if_available(&health_sdk) == nullptr);
    health_sdk.abi.size = sizeof(QolHostSdk);
    health_sdk.abi.major = 2;
    assert(module_health_if_available(&health_sdk) == nullptr);
    health_sdk.abi.major = 1;
    module_health_api.struct_size = offsetof(QolHostModuleHealthFactsApi, read);
    assert(module_health_if_available(&health_sdk) == nullptr);
    module_health_api.struct_size = sizeof(QolHostModuleHealthFactsApi);
    module_health_api.read = nullptr;
    assert(module_health_if_available(&health_sdk) == nullptr);
    module_health_api.read = &module_health_read_stub;
    health_sdk.module_health = nullptr;
    assert(module_health_if_available(&health_sdk) == nullptr);
    const QolConfigNamespaceApi namespace_v10{32, kConfigNamespaceMajor,
        kConfigNamespaceLegacyMinor,
        +[](QolOwnerToken, char*, uint32_t, uint32_t*) { return ok(); },
        +[](QolOwnerToken, const char*, uint32_t) { return ok(); },
        +[](QolOwnerToken) { return ok(); }, nullptr, nullptr};
    const QolConfigNamespaceApi namespace_api{sizeof(QolConfigNamespaceApi),
        kConfigNamespaceMajor, kConfigNamespaceMinor,
        +[](QolOwnerToken, char*, uint32_t, uint32_t*) { return ok(); },
        +[](QolOwnerToken, const char*, uint32_t) { return ok(); },
        +[](QolOwnerToken) { return ok(); },
        +[](QolOwnerToken, const char*, const char*, const char*, uint32_t) { return ok(); },
        +[](QolOwnerToken, char*, uint32_t, uint32_t*) { return ok(); }};
    assert(config_namespace_if_available(nullptr) == nullptr);
    assert(config_namespace_update_if_available(&namespace_v10) == nullptr);
    assert(config_namespace_registered_if_available(&namespace_v10) == nullptr);
    assert(config_namespace_update_if_available(&namespace_api) == &namespace_api);
    assert(config_namespace_registered_if_available(&namespace_api) == &namespace_api);
    sdk.config_namespace = &namespace_v10;
    sdk.abi.minor = 19;
    sdk.abi.size = sizeof(QolHostSdk);
    assert(config_namespace_if_available(&sdk) == nullptr);
    sdk.abi.minor = 20;
    assert(config_namespace_if_available(&sdk) == &namespace_v10);
    assert(config_namespace_v11_if_available(&sdk) == nullptr);
    sdk.config_namespace = &namespace_api;
    assert(config_namespace_if_available(&sdk) == &namespace_api);
    sdk.abi.minor = 21;
    assert(config_namespace_v11_if_available(&sdk) == &namespace_api);
    sdk.abi.size = offsetof(QolHostSdk, config_namespace);
    assert(config_namespace_if_available(&sdk) == nullptr);
    std::printf("SDK v1.15 prefix bytes=%zu, panels offset=%zu, snapshot_release offset=%zu\n",
                sizeof(HostSdkV15), offsetof(HostSdkV15, panels), offsetof(HostSdkV15, snapshot_release));
    std::printf("Settings catalog v1.3: legacy v1.2 size=%zu, new size=%zu, metadata tail offset=%zu\n",
                offsetof(QolSettingsCatalogApi, register_category_metadata),
                sizeof(QolSettingsCatalogApi), offsetof(QolSettingsCatalogApi, register_category_metadata));
    std::printf("Plugin descriptor: metadata=%zu, count=%zu, size=%zu\n",
                offsetof(PluginDescriptor, http_route_metadata),
                offsetof(PluginDescriptor, http_route_metadata_count), sizeof(PluginDescriptor));
    std::printf("Host SDK v1.26: settings_catalog=%zu, settings_config=%zu, runtime_effects=%zu, "
                "security=%zu, module_health=%zu, total=%zu\n",
                offsetof(QolHostSdk, settings_catalog), offsetof(QolHostSdk, settings_config),
                offsetof(QolHostSdk, runtime_effects), offsetof(QolHostSdk, security),
                offsetof(QolHostSdk, module_health), sizeof(QolHostSdk));
}
