#include "contracts/plugin.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

using namespace qol::contracts;
static_assert(kHostSdkMajor == 1 && kHostSdkMinor == 30);
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
static_assert(sizeof(void*) == 8, "supported Android/host targets use the 64-bit ABI");
static_assert(sizeof(HostSdkV15) == 744);
static_assert(offsetof(QolHostSdk, panels) == 248);
static_assert(offsetof(QolHostSdk, snapshot_release) == 736);
// v1.15 前缀（744 字节）之后为纯指针尾追加：minor 17 尾原为 settings_config（已随退役的
// settings catalog/config 链删除，后续尾偏移整体前移 16 字节），minor 18 尾（module_version）
// 及后续 owner/operation/route/runtime/security/module-health 尾只追加指针。
static_assert(offsetof(QolHostSdk, module_version) == 744);
static_assert(offsetof(QolHostSdk, owner_verify_token) == 752);
static_assert(offsetof(QolHostSdk, owner_store) == 760);
static_assert(offsetof(QolHostSdk, slot_operation) == 768);
static_assert(offsetof(QolHostSdk, save_route) == 776);
static_assert(offsetof(QolHostSdk, config_namespace) == 784);
static_assert(offsetof(QolHostSdk, feature) == 792);
static_assert(offsetof(QolHostSdk, feature_control) == 800);
static_assert(offsetof(QolHostSdk, runtime_effects) == 808);
static_assert(offsetof(QolHostSdk, security) == 816);
static_assert(offsetof(QolHostSdk, module_health) == 824);
static_assert(offsetof(QolHostSdk, desc_hook_v2) == 832);
static_assert(offsetof(QolHostSdk, panel_enumeration) == 840);
static_assert(sizeof(QolHostSdk) == 848);
static_assert(std::is_standard_layout_v<FeatureDefinition> &&
              std::is_trivially_copyable_v<FeatureDefinition>);
constexpr FeatureDefinition kDefaultPriorityFeature{};
static_assert(kDefaultPriorityFeature.display_priority == 0);
static_assert(kMinFeatureDisplayPriority == -10 && kMaxFeatureDisplayPriority == 10);
static_assert(offsetof(FeatureDefinition, action_label) == 48);
static_assert(offsetof(FeatureDefinition, display_priority) == 56);
static_assert(offsetof(FeatureDefinition, choices) == 64);
static_assert(offsetof(FeatureDefinition, choice_count) == 72);
static_assert(offsetof(FeatureDefinition, on_choice) == 80);
static_assert(sizeof(FeatureDefinition) == 88);
static_assert(sizeof(FeatureChoiceOption) == 2 * sizeof(const char*));
static_assert(offsetof(QolFeatureApi, read_choice) == 96);
static_assert(offsetof(QolFeatureApi, request_set_choice) == 104);
static_assert(sizeof(QolFeatureApi) == 112);
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
static_assert(offsetof(QolFeatureCatalogItem, choices) == 48);
static_assert(offsetof(QolFeatureCatalogItem, choice_count) == 56);
static_assert(sizeof(QolFeatureCatalogItem) == 64);
static_assert(offsetof(QolFeatureControlApi, read_categories) == 96);
static_assert(offsetof(QolFeatureControlApi, read_owner_availability) == 104);
static_assert(offsetof(QolFeatureControlApi, read_choice) == 112);
static_assert(offsetof(QolFeatureControlApi, request_set_choice) == 120);
static_assert(sizeof(QolFeatureControlApi) == 128);
using ExpectedReadCategoriesFn =
    QolResult (*)(void*, const FeatureCategoryDefinition**, uint32_t*);
using ExpectedReadOwnerAvailabilityFn =
    QolResult (*)(void*, const char*, FeatureOwnerAvailability*);
using ExpectedReadChoiceFn = QolResult (*)(void*, QolOwnerToken, const char*, const char*,
                                           char*, uint32_t, uint32_t*);
using ExpectedRequestSetChoiceFn = QolResult (*)(void*, QolOwnerToken, const char*, const char*,
                                                 const char*, FeatureReceipt*);
using ExpectedControlReadChoiceFn = QolResult (*)(void*, const char*, const char*, char*,
                                                  uint32_t, uint32_t*);
using ExpectedControlRequestSetChoiceFn = QolResult (*)(void*, const char*, const char*,
                                                        const char*, FeatureReceipt*);
static_assert(std::is_same_v<decltype(QolFeatureControlApi::read_categories),
                             ExpectedReadCategoriesFn>);
static_assert(std::is_same_v<decltype(QolFeatureControlApi::read_owner_availability),
                             ExpectedReadOwnerAvailabilityFn>);
static_assert(std::is_same_v<decltype(QolFeatureApi::read_choice), ExpectedReadChoiceFn>);
static_assert(std::is_same_v<decltype(QolFeatureApi::request_set_choice), ExpectedRequestSetChoiceFn>);
static_assert(std::is_same_v<decltype(QolFeatureControlApi::read_choice), ExpectedControlReadChoiceFn>);
static_assert(std::is_same_v<decltype(QolFeatureControlApi::request_set_choice),
                             ExpectedControlRequestSetChoiceFn>);
static_assert(static_cast<uint8_t>(FeatureOwnerAvailability::kAvailable) == 0 &&
              static_cast<uint8_t>(FeatureOwnerAvailability::kInstallFailed) == 1 &&
              static_cast<uint8_t>(FeatureOwnerAvailability::kActivationFailed) == 2 &&
              static_cast<uint8_t>(FeatureOwnerAvailability::kBlocked) == 3 &&
              static_cast<uint8_t>(FeatureOwnerAvailability::kUnsupported) == 4 &&
              static_cast<uint8_t>(FeatureOwnerAvailability::kInvalid) == 5 &&
              static_cast<uint8_t>(FeatureOwnerAvailability::kFailed) == 6);
static_assert(static_cast<uint8_t>(FeatureType::kToggle) == 0 &&
              static_cast<uint8_t>(FeatureType::kEntry) == 1 &&
              static_cast<uint8_t>(FeatureType::kChoice) == 2);
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
static_assert(offsetof(PluginDescriptor, deactivate_allowed) == 228);
static_assert(sizeof(PluginDescriptor) == 232);
static_assert(offsetof(QolConfigNamespaceApi, read_json) == 8);
static_assert(offsetof(QolConfigNamespaceApi, replace_json) == 16);
static_assert(offsetof(QolConfigNamespaceApi, clear) == 24);
static_assert(offsetof(QolConfigNamespaceApi, update_value) == 32);
static_assert(offsetof(QolConfigNamespaceApi, read_registered) == 40);
static_assert(offsetof(QolConfigNamespaceApi, update_http_request) == 48);
static_assert(sizeof(QolConfigNamespaceApi) == 56);

const char* test_module_version() { return "0.7.13"; }
QolResult module_health_read_stub(void*, QolModuleHealthSnapshot*) { return ok(); }
QolResult desc_hook_resolve_stub(uint32_t, QolDescButtonTarget*) { return ok(); }
QolResult desc_hook_bind_stub(uint32_t, const QolDescButtonTarget*, QolDescButtonProc, uint64_t*) {
    return ok();
}
QolResult desc_hook_validate_stub(uint64_t, void*, QolDescButtonTarget*, int32_t*) { return ok(); }
QolResult desc_hook_original_matches_stub(uint64_t, const char*, int32_t*) { return ok(); }
QolResult desc_hook_call_original_stub(uint64_t, void*) { return ok(); }
QolResult desc_hook_unbind_stub(uint64_t) { return ok(); }

int main() {
    QolHostSdk sdk{};
    sdk.abi = {sizeof(QolHostSdk), 1, 16};
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

    QolDescHookApi desc_hook_api{sizeof(QolDescHookApi), &desc_hook_resolve_stub,
        &desc_hook_bind_stub, &desc_hook_validate_stub, &desc_hook_original_matches_stub,
        &desc_hook_call_original_stub, &desc_hook_unbind_stub};
    QolHostSdk desc_sdk{};
    desc_sdk.abi = {sizeof(QolHostSdk), kHostSdkMajor, kHostSdkMinor};
    desc_sdk.desc_hook_v2 = &desc_hook_api;
    assert(desc_hook_v2_if_available(&desc_sdk) == &desc_hook_api);
    assert(desc_hook_v2_if_available(nullptr) == nullptr);
    desc_sdk.abi.minor = 26;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_sdk.abi.minor = 27;
    desc_sdk.abi.size = offsetof(QolHostSdk, desc_hook_v2);
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_sdk.abi.size = sizeof(QolHostSdk);
    desc_sdk.abi.major = 2;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_sdk.abi.major = kHostSdkMajor;
    desc_hook_api.struct_size = sizeof(QolDescHookApi) - 1;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_hook_api.struct_size = sizeof(QolDescHookApi);
    desc_hook_api.resolve = nullptr;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_hook_api.resolve = &desc_hook_resolve_stub;
    desc_hook_api.bind = nullptr;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_hook_api.bind = &desc_hook_bind_stub;
    desc_hook_api.validate = nullptr;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_hook_api.validate = &desc_hook_validate_stub;
    desc_hook_api.original_matches = nullptr;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_hook_api.original_matches = &desc_hook_original_matches_stub;
    desc_hook_api.call_original = nullptr;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_hook_api.call_original = &desc_hook_call_original_stub;
    desc_hook_api.unbind = nullptr;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);
    desc_hook_api.unbind = &desc_hook_unbind_stub;
    desc_sdk.desc_hook_v2 = nullptr;
    assert(desc_hook_v2_if_available(&desc_sdk) == nullptr);

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
    std::printf("Plugin descriptor: metadata=%zu, count=%zu, size=%zu\n",
                offsetof(PluginDescriptor, http_route_metadata),
                offsetof(PluginDescriptor, http_route_metadata_count), sizeof(PluginDescriptor));
    std::printf("Host SDK: runtime_effects=%zu, "
                "security=%zu, module_health=%zu, desc_hook_v2=%zu, total=%zu\n",
                offsetof(QolHostSdk, runtime_effects), offsetof(QolHostSdk, security),
                offsetof(QolHostSdk, module_health), offsetof(QolHostSdk, desc_hook_v2),
                sizeof(QolHostSdk));
}
