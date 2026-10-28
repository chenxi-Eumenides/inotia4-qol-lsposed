#include <cassert>
#include <cstddef>
#include <cstdint>

#include "game/access/item_handle_registry.h"
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
        return qol::contracts::kQolItemHandleStatusInvalidArgument;
    }
    ItemHandleError error{};
    const QolItemHandle handle = g_registry->register_item(item, provider, &error);
    if (handle.slot == 0) return registry_error_status(error);
    *out_handle = handle;
    return qol::contracts::kQolItemHandleStatusOk;
}

int32_t registry_lookup_item(void* item, QolItemHandle* out_handle, uint32_t* out_provider) {
    ++g_registry_lookup_calls;
    if (g_registry == nullptr || out_handle == nullptr) {
        return qol::contracts::kQolItemHandleStatusInvalidArgument;
    }
    if (out_provider != nullptr) *out_provider = 0;
    const QolItemHandle handle = g_registry->lookup_item(item, out_provider);
    if (handle.slot == 0) return qol::contracts::kQolItemHandleStatusNotFound;
    *out_handle = handle;
    return qol::contracts::kQolItemHandleStatusOk;
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

void test_provider_callback_same_item_reentry_is_not_repeated() {
    ItemHandleRegistry registry;
    g_registry = &registry;
    auto sdk = make_sdk();
    assert(inventory_native_hook_bind_sdk(&sdk));
    int item = 1;
    g_source_object = &item;
    g_source_handle = registry.register_item(&item, kSourceOwner);
    assert(g_source_handle.slot != 0);
    assert(qol::features::core_item::kBagSourceRegistryApi.register_source(&kTestBagSource).code ==
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

    assert(qol::features::core_item::kBagSourceRegistryApi.unregister_source(kSourceSpace).code ==
           kQolOk);
    inventory_native_hook_bind_sdk(nullptr);
}

}  // namespace

int main() {
    test_registry_invalidation_suspend_restore_commit();
    test_support_disabled_same_item_reentry_is_blocked();
    test_native_same_item_reentry_only_calls_backup_once();
    test_different_objects_allow_a_to_b_then_block_a();
    test_failed_backup_cleans_identity_for_retry();
    test_provider_callback_same_item_reentry_is_not_repeated();
    return 0;
}
