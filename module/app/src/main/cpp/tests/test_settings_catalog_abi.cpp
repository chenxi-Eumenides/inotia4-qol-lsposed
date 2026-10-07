#include "contracts/host_sdk.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

using namespace qol::contracts;
static_assert(kSettingsCatalogMajor == 1 && kSettingsCatalogMinor == 3);
static_assert(kHostSdkMajor == 1 && kHostSdkMinor == 18);

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
// minor 18 尾（module_version）及后续 owner/operation 尾只追加指针。
static_assert(offsetof(QolHostSdk, settings_config) == 752);
static_assert(offsetof(QolHostSdk, module_version) == 760);
static_assert(offsetof(QolHostSdk, owner_verify_token) == 768);
static_assert(offsetof(QolHostSdk, owner_store) == 776);
static_assert(offsetof(QolHostSdk, slot_operation) == 784);
static_assert(sizeof(QolHostSdk) == 792);
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
    assert(settings_config_if_available(&sdk) == nullptr);
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
    std::printf("SDK v1.15 prefix bytes=%zu, panels offset=%zu, snapshot_release offset=%zu\n",
                sizeof(HostSdkV15), offsetof(HostSdkV15, panels), offsetof(HostSdkV15, snapshot_release));
    std::printf("Settings catalog v1.3: legacy v1.2 size=%zu, new size=%zu, metadata tail offset=%zu\n",
                offsetof(QolSettingsCatalogApi, register_category_metadata),
                sizeof(QolSettingsCatalogApi), offsetof(QolSettingsCatalogApi, register_category_metadata));
    std::printf("Host SDK v1.18: settings_catalog offset=%zu, settings_config offset=%zu, total=%zu\n",
                offsetof(QolHostSdk, settings_catalog), offsetof(QolHostSdk, settings_config),
                sizeof(QolHostSdk));
}
