#include <cassert>
#include <cstddef>
#include <cstdint>

#include "contracts/game_cache.h"
#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_handle_registry.h"
#include "game/adapter_ports/game_host_port.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/ops/item_inventory_state_write.h"
#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.item/api/bag_source.h"
#include "plugins/core.item/hooks/inventory_hooks.cpp"

namespace {

using qol::contracts::QolItemHandle;
using qol::game::ItemHandleError;
using qol::game::ItemHandleRegistry;

constexpr uint32_t kSourceOwner = 7;
constexpr int32_t kSourceSpace = qol::contracts::kQolBagSpaceFirstRegistered;

ItemHandleRegistry* g_registry = nullptr;
int g_registry_lookup_calls = 0;
int g_registry_register_calls = 0;
int32_t g_registry_last_lookup_status = qol::contracts::kQolItemHandleStatusOk;
int32_t g_registry_last_register_status = qol::contracts::kQolItemHandleStatusOk;
void* g_ops_item = nullptr;
QolItemHandle g_ops_handle{};
int32_t g_ops_bag = 0;
int32_t g_ops_slot = 2;
uint32_t g_ops_count = 0;
int g_ops_count_write_calls = 0;
int g_ops_wrapper_entry_calls = 0;
int g_ops_backup_calls = 0;
int g_ops_itempool_free_calls = 0;
int g_ops_native_slot_set_calls = 0;
int g_ops_frame_refresh_calls = 0;
int g_ops_backup_result = 0;
int g_ops_nested_result = -1;
bool g_ops_nested_started = false;
bool g_ops_backup_reenter = false;
bool g_ops_transition_after_wrapper = false;
bool g_ops_lifecycle_transition_started = false;
bool g_ops_suspend_during_slot_scan = false;
bool g_ops_injected_suspend = false;
QolItemHandle g_ops_injected_checkpoint{};

int32_t registry_error_status(ItemHandleError error) {
    switch (error) {
        case ItemHandleError::kNone: return qol::contracts::kQolItemHandleStatusOk;
        case ItemHandleError::kWrongThread: return qol::contracts::kQolItemHandleStatusWrongThread;
        case ItemHandleError::kFull: return qol::contracts::kQolItemHandleStatusCapacity;
        case ItemHandleError::kType: return qol::contracts::kQolItemHandleStatusType;
        case ItemHandleError::kTransition: return qol::contracts::kQolItemHandleStatusTransition;
        case ItemHandleError::kInvalidArgument: return qol::contracts::kQolItemHandleStatusInvalidArgument;
        case ItemHandleError::kStale: return qol::contracts::kQolItemHandleStatusStale;
    }
    return qol::contracts::kQolItemHandleStatusInvalidArgument;
}

int32_t registry_register_item(void* item, uint32_t provider, QolItemHandle* out_handle) {
    ++g_registry_register_calls;
    if (g_registry == nullptr || out_handle == nullptr) {
        return g_registry_last_register_status =
                   qol::contracts::kQolItemHandleStatusInvalidArgument;
    }
    ItemHandleError error{};
    const QolItemHandle handle = g_registry->register_item(item, provider, &error);
    if (handle.slot == 0) return g_registry_last_register_status = registry_error_status(error);
    *out_handle = handle;
    return g_registry_last_register_status = qol::contracts::kQolItemHandleStatusOk;
}

int32_t registry_lookup_item(void* item, QolItemHandle* out_handle, uint32_t* out_provider) {
    ++g_registry_lookup_calls;
    if (g_registry == nullptr || out_handle == nullptr) {
        return g_registry_last_lookup_status =
                   qol::contracts::kQolItemHandleStatusInvalidArgument;
    }
    if (out_provider != nullptr) *out_provider = 0;
    const QolItemHandle handle = g_registry->lookup_item(item, out_provider);
    if (handle.slot == 0) {
        return g_registry_last_lookup_status = qol::contracts::kQolItemHandleStatusNotFound;
    }
    *out_handle = handle;
    return g_registry_last_lookup_status = qol::contracts::kQolItemHandleStatusOk;
}

int32_t registry_invalidate_item(void* item, uint32_t provider) {
    return g_registry != nullptr && g_registry->invalidate_item(item, provider)
               ? qol::contracts::kQolItemHandleStatusOk
               : qol::contracts::kQolItemHandleStatusNotFound;
}

int32_t registry_rebind_item(void* item, uint32_t provider) {
    return g_registry != nullptr && g_registry->rebind_item(item, provider)
               ? qol::contracts::kQolItemHandleStatusOk
               : qol::contracts::kQolItemHandleStatusNotFound;
}

int32_t registry_begin_invalidation(void* item, uint32_t provider,
                                    QolItemHandle* out_checkpoint) {
    return g_registry != nullptr &&
                   g_registry->begin_invalidation(item, provider, out_checkpoint)
               ? qol::contracts::kQolItemHandleStatusOk
               : qol::contracts::kQolItemHandleStatusNotFound;
}

int32_t registry_finish_invalidation(QolItemHandle checkpoint, int32_t committed) {
    return g_registry != nullptr &&
                   g_registry->finish_invalidation(checkpoint, committed != 0)
               ? qol::contracts::kQolItemHandleStatusOk
               : qol::contracts::kQolItemHandleStatusStale;
}

int32_t intercept_blocks(const char*, void*) { return 0; }
int32_t debug_disabled() { return 0; }

qol::contracts::QolHostSdk make_sdk() {
    qol::contracts::QolHostSdk sdk{};
    sdk.abi.size = static_cast<uint32_t>(offsetof(qol::contracts::QolHostSdk, item_handle) +
                                         sizeof(qol::contracts::QolItemHandleApi));
    sdk.abi.major = 1;
    sdk.abi.minor = 29;
    sdk.log.debug_enabled = &debug_disabled;
    sdk.hooks.intercept_blocks = &intercept_blocks;
    sdk.item_handle = {&registry_register_item, &registry_lookup_item,
                       &registry_invalidate_item, &registry_rebind_item,
                       &registry_begin_invalidation, &registry_finish_invalidation};
    return sdk;
}

enum class BackupMode { kReturn, kSameItemReentry, kAtoBtoA };

BackupMode g_backup_mode = BackupMode::kReturn;
void* g_item_a = nullptr;
void* g_item_b = nullptr;
int g_backup_calls_a = 0;
int g_backup_calls_b = 0;
int g_backup_result = 17;
int g_nested_same_result = -1;
int g_nested_b_result = -1;
int g_nested_a_result = -1;
bool g_same_reentry_started = false;
bool g_chain_started = false;

int test_backup(void* item) {
    if (item == g_item_a) ++g_backup_calls_a;
    if (item == g_item_b) ++g_backup_calls_b;

    if (g_backup_mode == BackupMode::kSameItemReentry && item == g_item_a &&
        !g_same_reentry_started) {
        g_same_reentry_started = true;
        g_nested_same_result = remove_item_wrapper(item);
        return 41;
    }
    if (g_backup_mode == BackupMode::kAtoBtoA && item == g_item_a && !g_chain_started) {
        g_chain_started = true;
        g_nested_b_result = remove_item_wrapper(g_item_b);
        return 41;
    }
    if (g_backup_mode == BackupMode::kAtoBtoA && item == g_item_b) {
        g_nested_a_result = remove_item_wrapper(g_item_a);
        return 23;
    }
    return g_backup_result;
}

void reset_backup(BackupMode mode) {
    g_backup_mode = mode;
    g_item_a = nullptr;
    g_item_b = nullptr;
    g_backup_calls_a = 0;
    g_backup_calls_b = 0;
    g_backup_result = 17;
    g_nested_same_result = -1;
    g_nested_b_result = -1;
    g_nested_a_result = -1;
    g_same_reentry_started = false;
    g_chain_started = false;
    g_backup_remove_item = &test_backup;
}

void reset_registry_counters() {
    g_registry_lookup_calls = 0;
    g_registry_register_calls = 0;
    g_registry_last_lookup_status = qol::contracts::kQolItemHandleStatusOk;
    g_registry_last_register_status = qol::contracts::kQolItemHandleStatusOk;
}

void register_native_item(ItemHandleRegistry& registry, void* item) {
    assert(registry.register_item(item, qol::contracts::kQolItemHandleOwnerGame).slot != 0);
}

void test_registry_invalidation_suspend_restore_commit() {
    ItemHandleRegistry registry;
    g_registry = &registry;
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));

    int item = 1;
    ItemHandleError error{};
    const QolItemHandle original = registry.register_item(&item, kSourceOwner, &error);
    assert(original.slot != 0 && error == ItemHandleError::kNone);

    QolItemHandle checkpoint{};
    assert(sdk.item_handle.begin_invalidation(&item, kSourceOwner, &checkpoint) ==
           qol::contracts::kQolItemHandleStatusOk);
    QolItemHandle found{};
    uint32_t owner = 0;
    assert(sdk.item_handle.lookup_item(&item, &found, &owner) ==
           qol::contracts::kQolItemHandleStatusNotFound);
    assert(sdk.item_handle.register_item(&item, kSourceOwner, &found) ==
           qol::contracts::kQolItemHandleStatusTransition);
    assert(sdk.item_handle.finish_invalidation(checkpoint, 0) ==
           qol::contracts::kQolItemHandleStatusOk);
    assert(sdk.item_handle.lookup_item(&item, &found, &owner) ==
           qol::contracts::kQolItemHandleStatusOk);
    assert(found.slot == original.slot && found.generation == original.generation);

    assert(sdk.item_handle.begin_invalidation(&item, kSourceOwner, &checkpoint) ==
           qol::contracts::kQolItemHandleStatusOk);
    assert(sdk.item_handle.lookup_item(&item, &found, &owner) ==
           qol::contracts::kQolItemHandleStatusNotFound);
    assert(sdk.item_handle.register_item(&item, kSourceOwner, &found) ==
           qol::contracts::kQolItemHandleStatusTransition);
    assert(sdk.item_handle.finish_invalidation(checkpoint, 1) ==
           qol::contracts::kQolItemHandleStatusOk);
    assert(sdk.item_handle.lookup_item(&item, &found, &owner) ==
           qol::contracts::kQolItemHandleStatusNotFound);
    assert(sdk.item_handle.register_item(&item, kSourceOwner, &found) ==
           qol::contracts::kQolItemHandleStatusOk);
    assert(found.slot == original.slot && found.generation != original.generation);
    inventory_native_hook_bind_sdk(nullptr);
}

void test_support_disabled_same_item_reentry_is_blocked() {
    ItemHandleRegistry registry;
    g_registry = &registry;
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 1;
    g_item_a = &item;
    reset_backup(BackupMode::kSameItemReentry);
    g_item_a = &item;
    reset_registry_counters();
    inventory_native_hook_set_support(false);

    assert(remove_item_wrapper(&item) == 41);
    assert(g_nested_same_result == 0);
    assert(g_backup_calls_a == 1);
    assert(g_registry_lookup_calls == 0 && g_registry_register_calls == 0);

    inventory_native_hook_set_support(true);
    inventory_native_hook_bind_sdk(nullptr);
}

void test_native_same_item_reentry_only_calls_backup_once() {
    ItemHandleRegistry registry;
    g_registry = &registry;
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 1;
    register_native_item(registry, &item);
    g_item_a = &item;
    reset_backup(BackupMode::kSameItemReentry);
    g_item_a = &item;
    reset_registry_counters();
    inventory_native_hook_set_support(true);

    assert(remove_item_wrapper(&item) == 41);
    assert(g_nested_same_result == 0);
    assert(g_backup_calls_a == 1);
    assert(g_registry_lookup_calls == 1 && g_registry_register_calls == 0);
    const QolItemHandle handle = registry.lookup_item(&item);
    assert(handle.slot != 0 && registry.resolve_item(handle).item == &item);

    inventory_native_hook_bind_sdk(nullptr);
}

void test_different_objects_allow_a_to_b_then_block_a() {
    ItemHandleRegistry registry;
    g_registry = &registry;
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item_a = 1;
    int item_b = 2;
    register_native_item(registry, &item_a);
    register_native_item(registry, &item_b);
    g_item_a = &item_a;
    g_item_b = &item_b;
    reset_backup(BackupMode::kAtoBtoA);
    g_item_a = &item_a;
    g_item_b = &item_b;
    reset_registry_counters();
    inventory_native_hook_set_support(true);

    assert(remove_item_wrapper(&item_a) == 41);
    assert(g_nested_b_result == 23);
    assert(g_nested_a_result == 0);
    assert(g_backup_calls_a == 1 && g_backup_calls_b == 1);
    assert(g_registry_lookup_calls == 2 && g_registry_register_calls == 0);

    inventory_native_hook_bind_sdk(nullptr);
}

void test_failed_backup_cleans_identity_for_retry() {
    ItemHandleRegistry registry;
    g_registry = &registry;
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 1;
    register_native_item(registry, &item);
    g_item_a = &item;
    reset_backup(BackupMode::kReturn);
    g_item_a = &item;
    g_backup_result = 0;
    inventory_native_hook_set_support(true);

    assert(remove_item_wrapper(&item) == 0);
    assert(g_backup_calls_a == 1);
    assert(registry.lookup_item(&item).slot != 0);
    g_backup_result = 9;
    assert(remove_item_wrapper(&item) == 9);
    assert(g_backup_calls_a == 2);

    inventory_native_hook_bind_sdk(nullptr);
}

void* g_source_object = nullptr;
QolItemHandle g_source_handle{};
int g_source_remove_calls = 0;
int g_source_nested_result = -1;
bool g_source_nested_started = false;

int32_t source_count() { return 1; }

QolResult source_info(int32_t, qol::contracts::QolBagInfo* out) {
    if (out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "null info", 9);
    }
    *out = {1, 1};
    return qol::contracts::ok();
}

QolResult source_item_at(
    int32_t bag, int32_t slot, qol::contracts::QolInventoryItemRef* out) {
    if (bag != 0 || slot != 0 || out == nullptr) {
        return qol::contracts::error(kQolErrNotFound, "item missing", 12);
    }
    *out = {0, 0, 0, 99, 1, g_source_handle, kSourceSpace};
    return qol::contracts::ok();
}

QolResult source_for_each(qol::contracts::QolInventoryBagSlotFn, void*) {
    return qol::contracts::ok();
}

QolResult source_export(QolItemHandle, int32_t, qol::contracts::QolBagTransfer*) {
    return qol::contracts::error(kQolErrUnsupported, "not used", 8);
}

QolResult source_import(const qol::contracts::QolBagTransfer*, int32_t, int32_t,
                        qol::contracts::QolBagMutationResult*) {
    return qol::contracts::error(kQolErrUnsupported, "not used", 8);
}

QolResult source_remove(QolItemHandle item, int32_t count,
                        qol::contracts::QolBagMutationResult*) {
    assert(item.slot == g_source_handle.slot && item.generation == g_source_handle.generation);
    assert(count == 1);
    ++g_source_remove_calls;
    if (!g_source_nested_started) {
        g_source_nested_started = true;
        g_source_nested_result = remove_item_wrapper(g_source_object);
    }
    return qol::contracts::ok();
}

QolResult source_identify(QolItemHandle item, qol::contracts::QolBagRef* out_ref,
                           int32_t* out_slot) {
    if (item.slot != g_source_handle.slot || item.generation != g_source_handle.generation ||
        out_ref == nullptr || out_slot == nullptr) {
        return qol::contracts::error(kQolErrNotFound, "item missing", 12);
    }
    *out_ref = {kSourceSpace, 0};
    *out_slot = 0;
    return qol::contracts::ok();
}

const qol::contracts::QolBagSource kTestBagSource{
    qol::contracts::kBagSourceVersion, kSourceSpace, &source_count, &source_info,
    &source_item_at, &source_for_each, &source_export, &source_import, &source_remove,
    &source_identify, nullptr};

QolResult ops_canonical_count(const void* item, uint32_t* out_count) {
    if (item != g_ops_item || out_count == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid test item count", 23);
    }
    *out_count = g_ops_count;
    return qol::contracts::ok();
}

QolResult ops_read_encoded_field(const void* item, uint32_t* out_field) {
    if (item != g_ops_item || out_field == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid test item field", 23);
    }
    *out_field = g_ops_count;
    return qol::contracts::ok();
}

QolResult ops_field_from_canonical(uint32_t, uint32_t canonical, int32_t,
                                   uint32_t* out_field) {
    if (out_field == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid test count output", 25);
    }
    *out_field = canonical;
    return qol::contracts::ok();
}

QolResult ops_write_encoded_field(void* item, uint32_t field) {
    if (item != g_ops_item) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid test item field", 23);
    }
    g_ops_count = field;
    ++g_ops_count_write_calls;
    return qol::contracts::ok();
}

const qol::contracts::QolItemCountApi kOpsCountApi = [] {
    qol::contracts::QolItemCountApi api{};
    api.canonical = &ops_canonical_count;
    api.field_from_canonical = &ops_field_from_canonical;
    api.read_encoded_field = &ops_read_encoded_field;
    api.write_encoded_field = &ops_write_encoded_field;
    return api;
}();

int ops_cumulate_count(void*) { return static_cast<int>(g_ops_count); }

int ops_test_backup(void* item) {
    ++g_ops_backup_calls;
    if (g_ops_backup_reenter && !g_ops_nested_started) {
        g_ops_nested_started = true;
        g_ops_nested_result = remove_item_wrapper(item);
    }
    if (g_ops_backup_result != 0 && item == g_ops_item) g_ops_item = nullptr;
    return g_ops_backup_result;
}

int ops_remove_through_wrapper(void* item) {
    ++g_ops_wrapper_entry_calls;
    const int result = remove_item_wrapper(item);
    if (g_ops_transition_after_wrapper) {
        g_ops_transition_after_wrapper = false;
        g_ops_lifecycle_transition_started =
            qol::game::item_handle_registry().begin_lifecycle_transition();
    }
    return result;
}

void ops_itempool_free(void*) { ++g_ops_itempool_free_calls; }

void prepare_ops_item(ItemHandleRegistry& registry, void* item, uint32_t count) {
    registry.invalidate_all();
    g_registry = &registry;
    g_ops_item = item;
    g_ops_bag = 0;
    g_ops_slot = 2;
    g_ops_count = count;
    g_ops_handle = registry.register_item(item, qol::contracts::kQolItemHandleOwnerGame);
    assert(g_ops_handle.slot != 0);
    g_ops_count_write_calls = 0;
    g_ops_wrapper_entry_calls = 0;
    g_ops_backup_calls = 0;
    g_ops_itempool_free_calls = 0;
    g_ops_native_slot_set_calls = 0;
    g_ops_frame_refresh_calls = 0;
    g_ops_backup_result = 0;
    g_ops_nested_result = -1;
    g_ops_nested_started = false;
    g_ops_backup_reenter = false;
    g_ops_transition_after_wrapper = false;
    g_ops_lifecycle_transition_started = false;
    g_ops_suspend_during_slot_scan = false;
    g_ops_injected_suspend = false;
    g_ops_injected_checkpoint = {};
    g_backup_remove_item = &ops_test_backup;
    fn_remove_item = &ops_remove_through_wrapper;
    fn_get_cumulate_count = &ops_cumulate_count;
    fn_itempool_free = &ops_itempool_free;
    inventory_native_hook_set_support(true);
    reset_registry_counters();
}

void register_source_for_ops_item() {
    g_source_object = g_ops_item;
    g_source_handle = g_ops_handle;
    g_source_remove_calls = 0;
    g_source_nested_result = -1;
    g_source_nested_started = false;
    assert(qol::features::core_inventory::kBagSourceRegistryApi.register_source(&kTestBagSource).code ==
           kQolOk);
}

void unregister_source_for_ops_item() {
    assert(qol::features::core_inventory::kBagSourceRegistryApi.unregister_source(kSourceSpace).code ==
           kQolOk);
}

void test_provider_callback_same_item_reentry_is_not_repeated() {
    ItemHandleRegistry registry;
    g_registry = &registry;
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 1;
    g_source_object = &item;
    g_source_handle = registry.register_item(&item, kSourceOwner);
    assert(g_source_handle.slot != 0);
    assert(qol::features::core_inventory::kBagSourceRegistryApi.register_source(&kTestBagSource).code ==
           kQolOk);
    g_source_remove_calls = 0;
    g_source_nested_result = -1;
    g_source_nested_started = false;
    reset_backup(BackupMode::kReturn);
    reset_registry_counters();
    inventory_native_hook_set_support(true);

    assert(remove_item_wrapper(&item) == 1);
    assert(g_source_nested_result == 0);
    assert(g_source_remove_calls == 1);
    assert(g_backup_calls_a == 0);
    assert(g_registry_lookup_calls == 1 && g_registry_register_calls == 0);

    assert(qol::features::core_inventory::kBagSourceRegistryApi.unregister_source(kSourceSpace).code ==
           kQolOk);
    inventory_native_hook_bind_sdk(nullptr);
}

void test_production_remove_operation_uses_wrapper_and_commits() {
    auto& registry = qol::game::item_handle_registry();
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 11;
    prepare_ops_item(registry, &item, 3);
    register_source_for_ops_item();
    g_ops_backup_result = 1;
    g_ops_backup_reenter = true;
    assert(fn_remove_item == &ops_remove_through_wrapper);

    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result = qol::game::ops::remove_item(g_ops_handle, 3, &out);

    assert(qol::contracts::succeeded(result));
    assert(out.value == 3);
    assert(g_ops_wrapper_entry_calls == 1 && g_ops_backup_calls == 1);
    assert(g_ops_nested_result == 0);
    assert(g_registry_lookup_calls == 1 && g_registry_register_calls == 1);
    assert(g_registry_last_lookup_status == qol::contracts::kQolItemHandleStatusNotFound);
    assert(g_registry_last_register_status == qol::contracts::kQolItemHandleStatusTransition);
    assert(g_source_remove_calls == 0);
    assert(g_ops_native_slot_set_calls == 0 && g_ops_itempool_free_calls == 0);
    assert(g_ops_item == nullptr);
    assert(registry.resolve_item(g_ops_handle).error == ItemHandleError::kStale);
    assert(g_ops_frame_refresh_calls == 1);

    unregister_source_for_ops_item();
    inventory_native_hook_bind_sdk(nullptr);
    fn_remove_item = nullptr;
}

void test_production_remove_operation_restores_failure_then_retries() {
    auto& registry = qol::game::item_handle_registry();
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 12;
    prepare_ops_item(registry, &item, 4);
    register_source_for_ops_item();
    g_ops_backup_result = 0;
    g_ops_backup_reenter = true;

    qol::contracts::QolItemInventoryStateWriteResult out{};
    QolResult result = qol::game::ops::remove_item(g_ops_handle, 4, &out);
    assert(result.code == kQolErrNotFound);
    assert(g_ops_wrapper_entry_calls == 1 && g_ops_backup_calls == 1);
    assert(g_ops_nested_result == 0);
    assert(g_registry_last_lookup_status == qol::contracts::kQolItemHandleStatusNotFound);
    assert(g_registry_last_register_status == qol::contracts::kQolItemHandleStatusTransition);
    const auto restored = registry.lookup_item(&item);
    assert(restored.slot == g_ops_handle.slot &&
           restored.generation == g_ops_handle.generation);
    assert(registry.resolve_item(g_ops_handle).item == &item);
    assert(g_ops_item == &item && g_ops_count == 4);
    assert(g_ops_native_slot_set_calls == 0 && g_ops_itempool_free_calls == 0);
    assert(g_ops_frame_refresh_calls == 0);

    g_ops_wrapper_entry_calls = 0;
    g_ops_backup_calls = 0;
    g_ops_nested_result = -1;
    g_ops_nested_started = false;
    g_ops_backup_result = 1;
    reset_registry_counters();
    result = qol::game::ops::remove_item(g_ops_handle, 4, &out);
    assert(qol::contracts::succeeded(result));
    assert(g_ops_wrapper_entry_calls == 1 && g_ops_backup_calls == 1);
    assert(g_ops_nested_result == 0);
    assert(g_source_remove_calls == 0);
    assert(g_ops_item == nullptr && g_ops_count == 4);
    assert(registry.resolve_item(g_ops_handle).error == ItemHandleError::kStale);
    assert(g_ops_native_slot_set_calls == 0 && g_ops_itempool_free_calls == 0);

    unregister_source_for_ops_item();
    inventory_native_hook_bind_sdk(nullptr);
    fn_remove_item = nullptr;
}

void test_production_remove_operation_finish_failure_does_not_restore_item() {
    auto& registry = qol::game::item_handle_registry();
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 17;
    prepare_ops_item(registry, &item, 2);
    register_source_for_ops_item();
    g_ops_backup_result = 1;
    g_ops_backup_reenter = true;
    g_ops_transition_after_wrapper = true;

    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result = qol::game::ops::remove_item(g_ops_handle, 2, &out);

    assert(result.code == kQolErrInternal);
    assert(g_ops_lifecycle_transition_started);
    assert(g_ops_wrapper_entry_calls == 1 && g_ops_backup_calls == 1);
    assert(g_ops_nested_result == 0);
    assert(g_ops_item == nullptr);
    assert(g_ops_native_slot_set_calls == 0 && g_ops_itempool_free_calls == 0);
    assert(g_source_remove_calls == 0);
    assert(g_ops_frame_refresh_calls == 0);
    assert(registry.resolve_item(g_ops_handle).error == ItemHandleError::kStale);

    registry.finish_lifecycle_transition(false);
    assert(registry.lookup_item(&item).slot == 0);
    ItemHandleError error{};
    assert(registry.register_item(&item, qol::contracts::kQolItemHandleOwnerGame, &error).slot ==
           0);
    assert(error == ItemHandleError::kTransition);
    // Cleanup finalizes the suspended invalidation; it does not restore the removed native item.
    assert(registry.finish_invalidation(g_ops_handle, true));
    assert(registry.resolve_item(g_ops_handle).error == ItemHandleError::kStale);
    assert(registry.lookup_item(&item).slot == 0);

    unregister_source_for_ops_item();
    inventory_native_hook_bind_sdk(nullptr);
    fn_remove_item = nullptr;
}

void test_production_remove_operation_finish_restore_failure_is_internal() {
    auto& registry = qol::game::item_handle_registry();
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 18;
    prepare_ops_item(registry, &item, 2);
    register_source_for_ops_item();
    g_ops_backup_result = 0;
    g_ops_transition_after_wrapper = true;

    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result = qol::game::ops::remove_item(g_ops_handle, 2, &out);

    assert(result.code == kQolErrInternal);
    assert(g_ops_lifecycle_transition_started);
    assert(g_ops_wrapper_entry_calls == 1 && g_ops_backup_calls == 1);
    assert(g_ops_item == &item && g_ops_count == 2);
    assert(g_ops_native_slot_set_calls == 0 && g_ops_itempool_free_calls == 0);
    assert(g_ops_frame_refresh_calls == 0);
    assert(registry.resolve_item(g_ops_handle).error == ItemHandleError::kStale);
    assert(registry.lookup_item(&item).slot == 0);

    registry.finish_lifecycle_transition(false);
    assert(registry.lookup_item(&item).slot == 0);
    assert(registry.finish_invalidation(g_ops_handle, false));
    assert(registry.resolve_item(g_ops_handle).item == &item);

    unregister_source_for_ops_item();
    inventory_native_hook_bind_sdk(nullptr);
    fn_remove_item = nullptr;
}

void test_production_remove_operation_prechecks_and_partial_count() {
    auto& registry = qol::game::item_handle_registry();
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    qol::contracts::QolItemInventoryStateWriteResult out{};

    int missing_fn_item = 13;
    prepare_ops_item(registry, &missing_fn_item, 3);
    fn_remove_item = nullptr;
    QolResult result = qol::game::ops::remove_item(g_ops_handle, 3, &out);
    assert(result.code == kQolErrNotReady);
    assert(g_ops_wrapper_entry_calls == 0 && g_ops_backup_calls == 0);
    assert(registry.resolve_item(g_ops_handle).item == &missing_fn_item);
    assert(g_ops_item == &missing_fn_item && g_ops_count == 3);
    assert(g_ops_native_slot_set_calls == 0 && g_ops_itempool_free_calls == 0);

    int stale_item = 14;
    prepare_ops_item(registry, &stale_item, 3);
    assert(registry.invalidate_item(&stale_item));
    result = qol::game::ops::remove_item(g_ops_handle, 3, &out);
    assert(result.code == kQolErrNotFound);
    assert(g_ops_wrapper_entry_calls == 0 && g_ops_backup_calls == 0);

    int transition_item = 15;
    prepare_ops_item(registry, &transition_item, 3);
    g_ops_suspend_during_slot_scan = true;
    result = qol::game::ops::remove_item(g_ops_handle, 3, &out);
    assert(result.code == kQolErrBlocked);
    assert(g_ops_injected_suspend);
    assert(g_ops_wrapper_entry_calls == 0 && g_ops_backup_calls == 0);
    assert(registry.lookup_item(&transition_item).slot == 0);
    assert(registry.finish_invalidation(g_ops_injected_checkpoint, false));
    assert(registry.resolve_item(g_ops_handle).item == &transition_item);
    assert(g_ops_item == &transition_item && g_ops_count == 3);

    int partial_item = 16;
    prepare_ops_item(registry, &partial_item, 3);
    result = qol::game::ops::remove_item(g_ops_handle, 1, &out);
    assert(qol::contracts::succeeded(result));
    assert(out.value == 1);
    assert(g_ops_count == 2 && g_ops_count_write_calls == 1);
    assert(g_ops_wrapper_entry_calls == 0 && g_ops_backup_calls == 0);
    assert(registry.resolve_item(g_ops_handle).item == &partial_item);
    assert(g_ops_item == &partial_item);
    assert(g_ops_frame_refresh_calls == 1);

    result = qol::game::ops::remove_item(g_ops_handle, 3, &out);
    assert(result.code == kQolErrInvalidArgument);
    assert(g_ops_wrapper_entry_calls == 0 && g_ops_backup_calls == 0);
    assert(g_ops_count == 2 && g_ops_count_write_calls == 1);
    assert(g_ops_item == &partial_item);

    inventory_native_hook_bind_sdk(nullptr);
    fn_remove_item = nullptr;
}

}  // namespace

namespace qol::game {

bool game_host_is_main_thread() { return true; }

NativeBagLayout native_bag_layout() {
    static uint8_t slots[6 * 0x80]{};
    return {slots, 6};
}

QolResult inventory_bag_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr || space_id != contracts::kQolBagSpaceOriginal) {
        return contracts::error(kQolErrNotReady, "native bag unavailable", 22);
    }
    *out_count = 6;
    return contracts::ok();
}

QolResult inventory_space_item_at(const contracts::QolBagRef* ref, int32_t slot,
                                  contracts::QolInventoryItemRef* out) {
    if (ref == nullptr || out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item lookup", 19);
    }
    if (ref->space_id != contracts::kQolBagSpaceOriginal || ref->bag != ::g_ops_bag ||
        slot != ::g_ops_slot || ::g_ops_item == nullptr) {
        return contracts::error(kQolErrNotFound, "native item not found", 21);
    }
    if (::g_ops_suspend_during_slot_scan && !::g_ops_injected_suspend) {
        ::g_ops_suspend_during_slot_scan = false;
        ::g_ops_injected_suspend = item_handle_registry().begin_invalidation(
            ::g_ops_item, contracts::kQolItemHandleOwnerAny,
            &::g_ops_injected_checkpoint);
    }
    *out = {0, ref->bag, slot, 99, static_cast<int32_t>(::g_ops_count),
            ::g_ops_handle, ref->space_id};
    return contracts::ok();
}

QolResult set_native_space_item(const contracts::QolBagRef* ref, int32_t slot, void* item) {
    ++::g_ops_native_slot_set_calls;
    if (ref == nullptr || ref->space_id != contracts::kQolBagSpaceOriginal ||
        ref->bag != ::g_ops_bag || slot != ::g_ops_slot) {
        return contracts::error(kQolErrNotFound, "native slot unavailable", 23);
    }
    ::g_ops_item = item;
    return contracts::ok();
}

const contracts::QolItemCountApi* item_count_api_if_resident() { return &::kOpsCountApi; }

}  // namespace qol::game

bool game_memory_accessible(const void* address, size_t size, char permission) {
    return address != nullptr && address == ::g_ops_item && size > 0 && permission == 'r';
}

bool game_in_world() { return true; }

void frame_cache_force_refresh() { ++::g_ops_frame_refresh_calls; }

decltype(fn_remove_item) fn_remove_item = nullptr;
decltype(fn_get_cumulate_count) fn_get_cumulate_count = nullptr;
decltype(fn_itempool_free) fn_itempool_free = nullptr;

int main() {
    qol::features::core_item::bind_bag_source_registry_for_test(
        &qol::features::core_inventory::kBagSourceRegistryApi);
    test_registry_invalidation_suspend_restore_commit();
    test_support_disabled_same_item_reentry_is_blocked();
    test_native_same_item_reentry_only_calls_backup_once();
    test_different_objects_allow_a_to_b_then_block_a();
    test_failed_backup_cleans_identity_for_retry();
    test_provider_callback_same_item_reentry_is_not_repeated();
    test_production_remove_operation_uses_wrapper_and_commits();
    test_production_remove_operation_restores_failure_then_retries();
    test_production_remove_operation_finish_failure_does_not_restore_item();
    test_production_remove_operation_finish_restore_failure_is_internal();
    test_production_remove_operation_prechecks_and_partial_count();
    return 0;
}
