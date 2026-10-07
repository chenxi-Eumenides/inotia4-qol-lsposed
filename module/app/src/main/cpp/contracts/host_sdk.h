#pragma once

// Host SDK ABI 骨架（ARCHITECTURE.md §2.1–§2.4、§3.8、§7.1；ADR-0008）。
//
// 这是插件访问宿主设施的唯一表类型。所有成员均为固定宽度 C 类型与函数指针，
// 不暴露 STL、异常、虚函数或宿主私有对象。内置表可随 P1–P4 迁移继续演进；P6
// 才冻结对外兼容边界。
//
// 各组替代关系：
//   log          → infra/log/qol_log.h
//   config       → infra/config.h
//   scheduler    → infra/scheduler.h
//   commands     → contracts/commands.h / infra/commands.h
//   events       → infra/events.h / contracts/events.h
//   capabilities → infra/capabilities.h
//   store        → infra/store.h（以调用方提供的 buffer/数组替代 STL 容器）
//   panels       → infra/panels.h / contracts/panels.h
//   hooks        → hooks/dispatch.h / hooks/popup_state.h
//   text_scope   → infra/text_scope.h
//
// hooks::registry/install/transaction、patch::*、game/access/patches、game/access/ptr_slots、
// game_symbols.h、host/* 与 patch/call_patch 均为内核私有，不进入本表。

#include <cstddef>
#include <cstdint>

#include "commands.h"
#include "http.h"
#include "in_flight.h"
#include "item_handle.h"
#include "owner_token.h"
#include "owner_store.h"
#include "lifecycle_participant.h"
#include "panels.h"
#include "result.h"
#include "settings.h"
#include "settings_config.h"
#include "version.h"

namespace qol::contracts {
struct QolSlotOperationLease;
struct QolSlotOperationApi;
struct QolSaveRouteApi;

struct QolAbiHeader {
    uint32_t size;
    uint16_t major;
    uint16_t minor;
};

using QolLogWriteFn = QolResult (*)(int32_t level, int32_t domain, const char* source,
                                    const char* message, uint32_t message_len);
using QolLogDebugEnabledFn = int32_t (*)();
struct QolLogApi {
    QolLogWriteFn write;
    QolLogDebugEnabledFn debug_enabled;
};

using QolConfigGetBoolFn = QolResult (*)(const char* key, int32_t default_value,
                                         int32_t* out_value);
using QolConfigGetI32Fn = QolResult (*)(const char* key, int32_t default_value,
                                        int32_t* out_value);
using QolConfigSetBoolFn = QolResult (*)(const char* key, int32_t value);
using QolConfigSetI32Fn = QolResult (*)(const char* key, int32_t value);
using QolConfigGetStringFn = QolResult (*)(const char* key, const char* default_value,
                                           char* out_value, uint32_t capacity,
                                           uint32_t* out_size);
struct QolConfigApi {
    QolConfigGetBoolFn get_bool;
    QolConfigGetI32Fn get_i32;
    QolConfigSetBoolFn set_bool;
    QolConfigSetI32Fn set_i32;
    QolConfigGetStringFn get_string;
};

using QolFrameHandler = int32_t (*)(int64_t frame, void* ctx);
using QolSchedulerAddFn = QolResult (*)(int32_t point, QolFrameHandler handler, void* ctx,
                                        int32_t interval, int32_t count, uint64_t* out_id);
using QolSchedulerRemoveFn = QolResult (*)(uint64_t id);
struct QolSchedulerApi {
    QolSchedulerAddFn add;
    QolSchedulerRemoveFn remove;
};

using QolEventHandler = void (*)(void* ctx, const void* payload);
using QolEventsSubscribeFn = QolResult (*)(uint32_t type, QolEventHandler handler, void* ctx,
                                           uint64_t* out_id);
using QolEventsUnsubscribeFn = QolResult (*)(uint64_t id);
struct QolEventsApi {
    QolEventsSubscribeFn subscribe;
    QolEventsUnsubscribeFn unsubscribe;
};

using QolCapabilityRequireFn = QolResult (*)(const char* id, QolVersionRange range,
                                             const void** out_iface);
using QolCapabilityLeaseEnterFn = QolResult (*)(const char* id, QolVersionRange range,
                                                uint64_t* out_lease);
using QolCapabilityLeaseLeaveFn = QolResult (*)(uint64_t lease);
using QolCapabilitySetAvailableFn = QolResult (*)(const char* provider_id, int32_t available);
struct QolCapabilitiesApi {
    QolCapabilityRequireFn require;
    QolCapabilityLeaseEnterFn lease_enter;
    QolCapabilityLeaseLeaveFn lease_leave;
    QolCapabilitySetAvailableFn set_available;
};

struct QolStoreSection {
    const char* name;
    uint16_t version;
    const uint8_t* data;
    uint32_t size;
};
struct QolStoreBuffer {
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
};
struct QolStoreParticipant {
    const char* name;
    bool (*prepare)(int32_t slot, const char* transaction_id);
    bool (*commit)(int32_t slot, const char* transaction_id);
    bool (*abort)(int32_t slot, const char* transaction_id);
};
using QolStoreReadFn = QolResult (*)(int32_t slot, const char* name, QolStoreBuffer* out,
                                     uint16_t* out_version);
using QolStoreWriteFn = QolResult (*)(int32_t slot, const char* name, uint16_t version,
                                      const uint8_t* data, uint32_t size);
using QolStoreRemoveFn = QolResult (*)(int32_t slot, const char* name);
using QolStoreReplaceSectionsFn = QolResult (*)(int32_t slot,
                                                 const QolStoreSection* replacements,
                                                 uint32_t replacement_count,
                                                 const char* const* removals,
                                                 uint32_t removal_count);
using QolStoreReadContainerFn = QolResult (*)(int32_t slot, uint8_t* out, uint32_t capacity,
                                              uint32_t* out_size);
using QolStoreImportContainerFn = QolResult (*)(int32_t slot, const uint8_t* data, uint32_t size);
using QolStoreDeleteSlotFn = QolResult (*)(int32_t slot);
using QolStoreRegisterParticipantFn = QolResult (*)(const QolStoreParticipant* participant);
using QolStoreSaveGameFn = QolResult (*)();
struct QolStoreApi {
    QolStoreReadFn read;
    QolStoreWriteFn write;
    QolStoreRemoveFn remove;
    QolStoreReplaceSectionsFn replace_sections;
    QolStoreReadContainerFn read_container;
    QolStoreImportContainerFn import_container;
    QolStoreDeleteSlotFn delete_slot;
    QolStoreRegisterParticipantFn register_participant;
    QolStoreSaveGameFn save_game;
};

static_assert(offsetof(QolStoreApi, read_container) == sizeof(QolStoreReadFn) * 4,
              "QolStoreApi section ABI layout changed");
static_assert(offsetof(QolStoreApi, import_container) == sizeof(QolStoreReadFn) * 5,
              "QolStoreApi container ABI layout changed");
static_assert(offsetof(QolStoreApi, delete_slot) == sizeof(QolStoreReadFn) * 6,
              "QolStoreApi container ABI layout changed");
static_assert(offsetof(QolStoreApi, register_participant) == sizeof(QolStoreReadFn) * 7,
              "QolStoreApi participant ABI layout changed");
static_assert(offsetof(QolStoreApi, save_game) == sizeof(QolStoreReadFn) * 8,
              "QolStoreApi save ABI layout changed");
static_assert(sizeof(QolStoreApi) == sizeof(QolStoreReadFn) * 9,
              "QolStoreApi ABI size changed");

// 只校验已持有的 token；签发、query 与 generation 递增为 Host 内部职责。
using QolOwnerVerifyTokenFn = QolResult (*)(QolOwnerToken token);
using QolOwnerStoreInspectFn = QolResult (*)(QolOwnerToken, int32_t, QolOwnerStoreSlotState*);
using QolOwnerStorePreflightFn = QolResult (*)(QolOwnerToken, int32_t,
    const QolOwnerStoreCandidate*, uint32_t, uint64_t*);
using QolOwnerStoreReserveFn = QolResult (*)(QolOwnerToken, int32_t,
    const QolOwnerStoreCandidate*, uint32_t, QolOwnerStoreReservation*);
using QolOwnerStoreReleaseFn = QolResult (*)(QolOwnerToken, QolOwnerStoreReservation);

// 命名空间绑定由装配期授权决定；Host 验证 owner 后转发。
// 迁移窗口保留旧 QolStoreApi，旧入口权限撤销由消费者迁移后收口。
struct QolOwnerStoreApi {
    QolAbiHeader abi;
    QolResult (*ensure_slot)(QolOwnerToken owner, int32_t slot);
    QolResult (*reset_slot)(QolOwnerToken owner, int32_t slot);
    QolResult (*read_section)(QolOwnerToken owner, int32_t slot, const char* section,
                             uint8_t* out_buffer, uint64_t capacity, uint64_t* out_size,
                             uint16_t* out_version);
    QolResult (*write_section)(QolOwnerToken owner, int32_t slot, const char* section,
                              uint16_t version, const uint8_t* bytes, uint64_t size);
    QolResult (*remove_section)(QolOwnerToken owner, int32_t slot, const char* section);
    // 先删 removals，再写 replacements，单次原子提交，其余 section 保留。
    QolResult (*replace_sections)(QolOwnerToken owner, int32_t slot,
                                 const QolStoreSection* replacements, uint32_t replacement_count,
                                 const char* const* removals, uint32_t removal_count);
    QolOwnerStoreInspectFn inspect_slot;
    // out_encoded_bytes 为 candidate_count 项的输出数组；候选各自基于同一当前容器。
    QolOwnerStorePreflightFn preflight;
    QolOwnerStoreReserveFn reserve;
    QolOwnerStoreReleaseFn release;
    // 显式凭据是跨调用事务身份，不能用线程、TLS 或写入顺序推断。
    // 成功提交后更新 in/out 凭据 generation；旧副本继续提交返回 Busy。
    // retain 是调用方声明的在途/恢复引用，不由 Host 解释 journal stage。
    QolResult (*write_reserved)(QolOwnerToken, int32_t, QolOwnerStoreReservation*,
                               const char*, uint16_t, const uint8_t*, uint64_t);
    QolResult (*remove_reserved)(QolOwnerToken, int32_t, QolOwnerStoreReservation*, const char*);
    QolResult (*replace_reserved)(QolOwnerToken, int32_t, QolOwnerStoreReservation*,
                                 const QolStoreSection*, uint32_t, const char* const*, uint32_t);
    // 原版调用前登记在途引用，不需为了改变租约引用而重复写 journal。
    QolResult (*set_referenced)(QolOwnerToken, int32_t, QolOwnerStoreReservation*, uint32_t);
    QolResult (*reserve_in_operation)(QolOwnerToken, const QolSlotOperationLease*, int32_t,
                                      const QolOwnerStoreCandidate*, uint32_t, QolOwnerStoreReservation*);
    QolResult (*ensure_in_operation)(QolOwnerToken, const QolSlotOperationLease*, int32_t);
    QolResult (*reset_in_operation)(QolOwnerToken, const QolSlotOperationLease*, int32_t);
};

using QolPanelFindFn = QolResult (*)(const char* panel_id, const PanelProviderSpec** out_provider);
using QolPanelRegisterItemFn = QolResult (*)(const PanelItemSpec* item);
using QolPanelRegisterProviderFn = QolResult (*)(const PanelProviderSpec* provider);
using QolPanelUnregisterOwnerFn = QolResult (*)(const char* owner);
using QolPanelOpenFn = QolResult (*)(const char* panel_id);
using QolPanelCloseFn = QolResult (*)(const char* panel_id);
using QolPanelActiveFn = QolResult (*)(const char* panel_id, int32_t* out_active);
struct QolPanelsApi {
    QolPanelRegisterItemFn register_item;
    QolPanelRegisterProviderFn register_provider;
    QolPanelUnregisterOwnerFn unregister_owner;
    QolPanelFindFn find;
    QolPanelOpenFn open;
    QolPanelCloseFn close;
    QolPanelActiveFn active;
};

using QolResultWriteOkFn = QolResult (*)(char* out, uint32_t capacity, uint32_t* out_size);
using QolResultWriteErrorFn = QolResult (*)(const char* message, char* out, uint32_t capacity,
                                            uint32_t* out_size);
struct QolResultApi {
    QolResultWriteOkFn write_ok;
    QolResultWriteErrorFn write_error;
};

using QolTextDispatchFn = const char* (*)(uint32_t text_id, const char* current, void* ctx);
using QolHooksTextSubscribeFn = QolResult (*)(const char* resource_id, int32_t phase,
                                              int32_t kind, QolTextDispatchFn fn, void* ctx,
                                              uint64_t* out_id);
using QolHooksTextUnsubscribeFn = QolResult (*)(const char* resource_id, uint64_t id);
using QolHooksRunTextFn = QolResult (*)(const char* resource_id, uint32_t text_id,
                                        const char* original, const char** out_text);
using QolHooksSetInterceptFn = QolResult (*)(const char* resource_id,
                                              int32_t (*predicate)(void* ctx), void* ctx);
using QolHooksClearInterceptFn = QolResult (*)(const char* resource_id);
using QolHooksInterceptBlocksFn = int32_t (*)(const char* resource_id, void* ctx);

// PopupState 的跨边界 POD 镜像。backup 必须完整保留原版 0x40 字节状态，slot/state_id
// 仅由宿主定位并填充；插件只持有该快照并通过 hooks API 交还宿主。
struct QolPopupStateHooks {
    void (*enter)();
    void (*process)();
    void (*f3)();
    void (*f4)();
    uint32_t (*event)(uint64_t, uint64_t, uint64_t);
};

struct QolPopupStateEntry {
    uint8_t backup[0x40];
    uint8_t* slot;
    int32_t state_id;
};

// 动态 UI 描述按钮槽的类型化绑定句柄。槽地址、写入和还原均由宿主执行。
struct QolDynamicDescHook {
    uint64_t handle;
    void* original;
};

using QolHooksPopupResolveFn = QolResult (*)(const char* symbol, QolPopupStateEntry* out_entry);
using QolHooksPopupAcquireFn = QolResult (*)(QolPopupStateEntry* entry,
                                              const QolPopupStateHooks* hooks);
using QolHooksPopupRestoreFn = QolResult (*)(QolPopupStateEntry* entry);
using QolHooksDescBindFn = QolResult (*)(uint32_t index, void* replacement,
                                         QolDynamicDescHook* out_hook);
using QolHooksDescUnbindFn = QolResult (*)(QolDynamicDescHook* hook);
using QolHooksDescMatchesButtonFn = QolResult (*)(const QolDynamicDescHook* hook,
                                                  void* button, int32_t* out_matches);
using QolHooksDescOriginalMatchesFn = QolResult (*)(const QolDynamicDescHook* hook,
                                                    const char* symbol, int32_t* out_matches);
using QolHooksDescCallOriginalFn = QolResult (*)(const QolDynamicDescHook* hook, void* button);
using QolHooksCallOriginalControlProcFn = QolResult (*)(void* control, uint64_t event,
                                                       void* x2, void* param,
                                                       uint64_t* out_result);
struct QolHooksApi {
    QolHooksTextSubscribeFn text_subscribe;
    QolHooksTextUnsubscribeFn text_unsubscribe;
    QolHooksRunTextFn run_text;
    QolHooksSetInterceptFn set_intercept;
    QolHooksClearInterceptFn clear_intercept;
    QolHooksInterceptBlocksFn intercept_blocks;
    QolHooksPopupResolveFn popup_resolve;
    QolHooksPopupAcquireFn popup_acquire;
    QolHooksPopupRestoreFn popup_restore;
    QolHooksDescBindFn desc_bind;
    QolHooksDescUnbindFn desc_unbind;
    QolHooksDescMatchesButtonFn desc_matches_button;
    QolHooksDescOriginalMatchesFn desc_original_matches;
    QolHooksDescCallOriginalFn desc_call_original;
    QolHooksCallOriginalControlProcFn call_original_control_proc;
};

using QolTextScopePushFn = QolResult (*)(int32_t scope);
using QolTextScopePopFn = QolResult (*)();
struct QolTextScopeApi {
    QolTextScopePushFn push;
    QolTextScopePopFn pop;
};

struct QolUiAdapter;

using QolDescButtonProc = void (*)(void* button);
struct QolDescButtonApi {
    // 在 index(0..10) 的执行槽上安装 proc，返回安装前的原函数入口；失败返回非 0 错误码。
    QolResult (*bind)(uint32_t index, QolDescButtonProc proc, void** out_original);
    // 卸载 index 槽；未安装时成功幂等。
    QolResult (*unbind)(uint32_t index);
    // 读取 index 槽当前指针（供识别"正在执行的是哪个槽"）。
    QolResult (*current_proc)(uint32_t index, void** out_proc);
    // original 是否等于某登记符号解析出的入口（symbol 名如 "F_UIEQUIP_BUTTON_USE_EXE_VMA"）；out_match=0/1。
    QolResult (*original_matches)(void* original, const char* symbol, int32_t* out_match);
    // 以 button 参数调用原函数入口。
    QolResult (*call_original)(void* original, void* button);
};

using QolSnapshotHandle = void*;
using QolSnapshotToken = void*;

// 模块身份版本（Kotlin BuildConfig.VERSION_NAME，启动期经 JNI 下发到 native）。
// 未下发时返回 nullptr；返回的字符串为静态存储，进程内有效。
using QolModuleVersionFn = const char* (*)();

struct QolHostSdk {
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
    // Tail extension; older plugins use abi.size to consume the prefix they know.
    QolHttpDispatchApi http_dispatch;
    // 票据结果回取面（阶段 5 追加；调用方须以 abi.size/abi.minor 协商后再消费）。
    QolTicketResultApi ticket_result;
    // 通用同步存档/UI 生命周期参与者注册（尾追加）。
    QolLifecycleParticipantRegisterFn lifecycle_participant_register;
    QolItemHandleApi item_handle;
    // Opaque immutable byte snapshots; the view remains valid until snapshot_release.
    QolSnapshotHandle (*snapshot_create)(uint32_t byte_size);
    void (*snapshot_destroy)(QolSnapshotHandle handle);
    bool (*snapshot_publish)(QolSnapshotHandle handle, const void* data, uint32_t byte_size);
    bool (*snapshot_acquire)(QolSnapshotHandle handle, const void** out_data, uint32_t* out_size,
                             QolSnapshotToken* out_token);
    void (*snapshot_release)(QolSnapshotToken token);
    // Independent resident declaration catalog; does not require core.settings to be installed.
    const QolSettingsCatalogApi* settings_catalog;
    const QolSettingsConfigApi* settings_config;
    // 模块身份版本访问（阶段 5 尾追加；调用方须以 abi.size/abi.minor 协商后再消费）。
    QolModuleVersionFn module_version;
    // 尾部扩展：以 abi.size 协商，不改变旧 SDK 前缀或存储表。
    QolOwnerVerifyTokenFn owner_verify_token;
    const QolOwnerStoreApi* owner_store;
    const QolSlotOperationApi* slot_operation;
    const QolSaveRouteApi* save_route;
};

constexpr uint16_t kHostSdkMajor = 1;
// minor 19：在 slot_operation 之后尾追加 save_route（core.save 领域回调路由）。
constexpr uint16_t kHostSdkMinor = 19;

inline const QolSettingsConfigApi* settings_config_if_available(const QolHostSdk* sdk) {
    constexpr std::size_t end = offsetof(QolHostSdk, settings_config) +
                                sizeof(const QolSettingsConfigApi*);
    if (sdk == nullptr || sdk->abi.major != kHostSdkMajor || sdk->abi.minor < 17 ||
        sdk->abi.size < end) return nullptr;
    const auto* api = sdk->settings_config;
    if (api == nullptr || api->size < sizeof(QolSettingsConfigApi) ||
        api->major != kSettingsConfigMajor || api->minor < kSettingsConfigMinor ||
        api->read_bool == nullptr || api->request_set_bool == nullptr ||
        api->query_request == nullptr) return nullptr;
    return api;
}

// Missing/older tail means optional contribution is unavailable, not plugin failure.
inline const QolSettingsCatalogApi* settings_catalog_if_available(const QolHostSdk* sdk) {
    constexpr std::size_t end = offsetof(QolHostSdk, settings_catalog) +
                                sizeof(const QolSettingsCatalogApi*);
    if (sdk == nullptr || sdk->abi.major != kHostSdkMajor || sdk->abi.minor < 16 ||
        sdk->abi.size < end) return nullptr;
    const auto* api = sdk->settings_catalog;
    constexpr std::size_t required = offsetof(QolSettingsCatalogApi, invoke_action) +
                                     sizeof(QolSettingsCatalogApi::invoke_action);
    if (api == nullptr || api->size < required ||
        api->major != kSettingsCatalogMajor ||
        api->register_category == nullptr || api->unregister_owner == nullptr ||
        api->snapshot_acquire == nullptr || api->snapshot_release == nullptr ||
        api->invoke_action == nullptr) return nullptr;
    return api;
}

inline const QolSettingsCatalogApi* settings_catalog_availability_if_available(
    const QolHostSdk* sdk) {
    const auto* api = settings_catalog_if_available(sdk);
    constexpr std::size_t end = offsetof(QolSettingsCatalogApi, query_item_availability) +
                                sizeof(QolSettingsCatalogApi::query_item_availability);
    if (api == nullptr || api->minor < 1 || api->size < end ||
        api->query_item_availability == nullptr) return nullptr;
    return api;
}

inline const QolSettingsCatalogApi* settings_catalog_join_if_available(const QolHostSdk* sdk) {
    const auto* api = settings_catalog_if_available(sdk);
    constexpr std::size_t end = offsetof(QolSettingsCatalogApi, register_items) +
                                sizeof(QolSettingsCatalogApi::register_items);
    if (api == nullptr || api->minor < 2 || api->size < end || api->register_items == nullptr) {
        return nullptr;
    }
    return api;
}

inline const QolSettingsCatalogApi* settings_catalog_metadata_if_available(const QolHostSdk* sdk) {
    const auto* api = settings_catalog_if_available(sdk);
    constexpr std::size_t end = offsetof(QolSettingsCatalogApi, register_category_metadata) +
                                sizeof(QolSettingsCatalogApi::register_category_metadata);
    if (api == nullptr || api->minor < 3 || api->size < end ||
        api->register_category_metadata == nullptr) return nullptr;
    return api;
}

// Missing/older tail means the identity is unavailable, not a fatal error：调用方保留无版本形态。
inline const char* host_sdk_module_version(const QolHostSdk* sdk) {
    constexpr std::size_t end = offsetof(QolHostSdk, module_version) + sizeof(QolModuleVersionFn);
    if (sdk == nullptr || sdk->abi.major != kHostSdkMajor || sdk->abi.minor < 18 ||
        sdk->abi.size < end || sdk->module_version == nullptr) return nullptr;
    return sdk->module_version();
}

}  // namespace qol::contracts
