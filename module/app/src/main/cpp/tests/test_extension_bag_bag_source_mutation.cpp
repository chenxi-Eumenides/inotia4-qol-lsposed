#include "contracts/bag.h"
#include "plugins/extension_bag/api/api_impl.h"
#include "plugins/extension_bag/api/bag_source.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/core/item_display_hook.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/core/observation.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

int g_pass = 0;
int g_fail = 0;
void* g_handle_item = nullptr;
qol::contracts::QolItemHandle g_public_handle{41, 19};
int g_materialize_calls = 0;
void* g_effective_item = nullptr;
uint32_t g_effective_count = 0;
int g_extension_free_calls = 0;
int g_persist_calls = 0;
bool g_reject_next_persist = false;

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (condition) {                                                           \
            ++g_pass;                                                              \
        } else {                                                                   \
            ++g_fail;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                          \
    } while (false)

virtual_bag::Item make_item(int category, int count) {
    virtual_bag::Item item{};
    item.category = category;
    item.count = count;
    item.payload_size = static_cast<uint16_t>(virtual_bag::kPayloadHeaderSize);
    item.payload[0] = static_cast<uint8_t>(item.payload_size - 1);
    std::memcpy(item.payload.data() + virtual_bag::kPayloadCountOffset, &count,
                sizeof(count));
    return item;
}

void make_transfer(int category, int count, qol::contracts::QolBagTransfer* out) {
    *out = {};
    const virtual_bag::Item item = make_item(category, count);
    out->category = category;
    out->count = count;
    out->payload_size = item.payload_size;
    std::memcpy(out->payload, item.payload.data(), item.payload_size);
}

bool same_item(const virtual_bag::Item& lhs, const virtual_bag::Item& rhs) {
    return lhs.category == rhs.category && lhs.count == rhs.count &&
           lhs.payload_size == rhs.payload_size && lhs.payload == rhs.payload;
}

bool same_ledger(const ownership::Ledger& lhs, const ownership::Ledger& rhs) {
    if (lhs.total_allocated != rhs.total_allocated ||
        lhs.total_released != rhs.total_released ||
        lhs.total_handed_over != rhs.total_handed_over) {
        return false;
    }
    for (int i = 0; i < ownership::kLedgerCapacity; ++i) {
        if (lhs.slots[i].state != rhs.slots[i].state ||
            lhs.slots[i].generation != rhs.slots[i].generation) {
            return false;
        }
    }
    return true;
}

void reset_fixture() {
    auto& runtime = runtime_state();
    runtime.objects.bag = virtual_bag::State{};
    runtime.objects.bag.types[0] = static_cast<uint8_t>(virtual_bag::BagType::kLargeBackpack);
    runtime.objects.bag.capacities[0] = virtual_bag::kSlotCount;
    runtime.objects.bag.bag_count = 5;
    runtime.persistence.dirty = false;
    runtime.enabled.store(true, std::memory_order_release);

    qol::extension_bag::ui_state().objects = ExtensionBagObjectUiState{};
    qol::extension_bag::ui_state().ui = ExtensionBagUiState{};
    g_active_module_use_token = nullptr;
    g_handle_item = nullptr;
    g_materialize_calls = 0;
    g_effective_item = nullptr;
    g_effective_count = 0;
    g_extension_free_calls = 0;
    g_persist_calls = 0;
    g_reject_next_persist = false;
}

bool ledger_has_single_module_object(const ownership::Ledger& ledger, uint32_t handle) {
    const ownership::Audit audit = ownership::audit(ledger);
    return ownership::live_state(ledger, handle, ownership::State::kModuleOwned) &&
           audit.balanced && audit.live_handles == 1 && audit.outstanding_objects == 1 &&
           audit.outstanding_borrows == 0 && ledger.total_allocated == 1 &&
           ledger.total_released == 0 && ledger.total_handed_over == 0;
}

void test_free_rejection_preserves_slot_and_active_use_custody() {
    reset_fixture();
    auto& bag = runtime_state().objects.bag;
    auto& objects = qol::extension_bag::ui_state().objects;
    constexpr int kBag = 0;
    constexpr int kSlot = 0;
    constexpr int kCategory = 77;
    int item_object = 1;
    bag.items[kBag][kSlot] = make_item(kCategory, 3);
    objects.module_objects[kBag][kSlot] = &item_object;
    objects.module_object_categories[kBag][kSlot] = kCategory;
    objects.module_object_hashes[kBag][kSlot] =
        virtual_bag::payload_hash(bag.items[kBag][kSlot]);
    g_handle_item = &item_object;

    uint32_t internal_handle = 0;
    CHECK(ownership::allocate(&objects.ownership_ledger, &internal_handle) ==
          ownership::Outcome::kOk);
    objects.module_object_handles[kBag][kSlot] = internal_handle;

    ModuleUseToken token{};
    CHECK(module_use_begin_locked(kBag, kSlot, &item_object, &token));
    g_active_module_use_token = &token;
    const virtual_bag::Item descriptor_before = bag.items[kBag][kSlot];
    const uint32_t hash_before = objects.module_object_hashes[kBag][kSlot];
    const uint32_t handle_before = objects.module_object_handles[kBag][kSlot];
    const ModuleUseState use_before = objects.module_object_use[kBag][kSlot];
    const ownership::Ledger ledger_before = objects.ownership_ledger;
    const uint64_t generation_before = use_before.generation;

    qol::contracts::QolBagTransfer transfer{};
    make_transfer(kCategory, 1, &transfer);
    qol::contracts::QolBagMutationResult mutation{};
    const qol::contracts::QolBagSource* source =
        qol::features::extension_bag::extension_bag_bag_source();
    const QolResult result = source->import_item(&transfer, kBag, kSlot, &mutation);

    CHECK(!qol::contracts::succeeded(result));
    CHECK(result.code == kQolErrConflict);
    CHECK(mutation.ref.space_id == 0 && mutation.ref.bag == 0 &&
          mutation.item.slot == 0 && mutation.item.generation == 0 && mutation.count == 0);
    CHECK(same_item(bag.items[kBag][kSlot], descriptor_before));
    CHECK(bag.items[kBag][kSlot].count == 3);
    CHECK(objects.module_object_hashes[kBag][kSlot] == hash_before);
    CHECK(objects.module_object_categories[kBag][kSlot] == kCategory);
    CHECK(objects.module_objects[kBag][kSlot] == &item_object);
    CHECK(objects.module_object_handles[kBag][kSlot] == handle_before);
    CHECK(objects.module_object_use[kBag][kSlot].generation == generation_before);
    CHECK(g_active_module_use_token == &token);
    CHECK(objects.module_object_use[kBag][kSlot].active == use_before.active);
    CHECK(objects.module_object_use[kBag][kSlot].consumed == use_before.consumed);
    CHECK(objects.module_object_use[kBag][kSlot].pending_release == use_before.pending_release);
    CHECK(objects.module_object_use[kBag][kSlot].item == use_before.item);
    CHECK(objects.module_object_use[kBag][kSlot].owner == use_before.owner);
    CHECK(token.bag == kBag && token.slot == kSlot && token.item == &item_object);
    CHECK(token.generation == generation_before && token.owner == use_before.owner);
    CHECK(ledger_has_single_module_object(objects.ownership_ledger, internal_handle));
    CHECK(ownership::live_state(ledger_before, internal_handle,
                                ownership::State::kModuleOwned));

    qol::contracts::QolInventoryItemRef ref{};
    const QolResult read_result = source->item_at(kBag, kSlot, &ref);
    CHECK(qol::contracts::succeeded(read_result));
    CHECK(ref.bag == kBag && ref.slot == kSlot && ref.category == kCategory && ref.count == 3);
    CHECK(ref.item.slot == g_public_handle.slot &&
          ref.item.generation == g_public_handle.generation);
    CHECK(ref.space_id == qol::features::extension_bag::kExtensionBagSpaceId);
    qol::contracts::QolItemHandle mapped_handle{};
    CHECK(extension_bag_item_handle_for_pointer(&item_object, &mapped_handle));
    CHECK(mapped_handle.slot == g_public_handle.slot &&
          mapped_handle.generation == g_public_handle.generation);
    qol::contracts::QolBagInfo info{};
    CHECK(qol::contracts::succeeded(source->info(kBag, &info)));
    CHECK(info.capacity == virtual_bag::kSlotCount && info.occupied_slots == 1);

    g_active_module_use_token = nullptr;
    CHECK(module_use_abort_locked(token));
    CHECK(!objects.module_object_use[kBag][kSlot].active);
}

void test_materialize_rejection_restores_empty_descriptor_and_handle_state() {
    reset_fixture();
    auto& bag = runtime_state().objects.bag;
    auto& objects = qol::extension_bag::ui_state().objects;
    constexpr int kBag = 0;
    constexpr int kSlot = 2;
    constexpr int kCategory = 88;
    objects.module_object_use[kBag][kSlot].generation = 23;
    const virtual_bag::Item descriptor_before = bag.items[kBag][kSlot];
    const ownership::Ledger ledger_before = objects.ownership_ledger;

    qol::contracts::QolBagTransfer transfer{};
    make_transfer(kCategory, 1, &transfer);
    qol::contracts::QolBagMutationResult mutation{};
    const qol::contracts::QolBagSource* source =
        qol::features::extension_bag::extension_bag_bag_source();
    const QolResult result = source->import_item(&transfer, kBag, kSlot, &mutation);

    CHECK(!qol::contracts::succeeded(result));
    CHECK(result.code == kQolErrInternal);
    CHECK(mutation.ref.space_id == 0 && mutation.ref.bag == 0 &&
          mutation.item.slot == 0 && mutation.item.generation == 0 && mutation.count == 0);
    CHECK(g_materialize_calls == 1);
    CHECK(same_item(bag.items[kBag][kSlot], descriptor_before));
    CHECK(bag.items[kBag][kSlot].category == 0 && bag.items[kBag][kSlot].count == 0 &&
          bag.items[kBag][kSlot].payload_size == 0);
    CHECK(objects.module_objects[kBag][kSlot] == nullptr);
    CHECK(objects.module_object_categories[kBag][kSlot] == 0);
    CHECK(objects.module_object_hashes[kBag][kSlot] == 0);
    CHECK(objects.module_object_handles[kBag][kSlot] == 0);
    // Production module_item_locked first rejects the missing native materialization;
    // the callback rollback then advances the empty slot generation once more.
    CHECK(objects.module_object_use[kBag][kSlot].generation == 25);
    CHECK(!objects.module_object_use[kBag][kSlot].active);
    CHECK(!objects.module_object_use[kBag][kSlot].pending_release);
    CHECK(objects.module_object_use[kBag][kSlot].item == nullptr);
    CHECK(objects.ownership_ledger.total_allocated == ledger_before.total_allocated);
    CHECK(objects.ownership_ledger.total_released == ledger_before.total_released);
    CHECK(objects.ownership_ledger.total_handed_over == ledger_before.total_handed_over);
    CHECK(ownership::audit(objects.ownership_ledger).live_handles == 0);

    qol::contracts::QolBagInfo info{};
    CHECK(qol::contracts::succeeded(source->info(kBag, &info)));
    CHECK(info.occupied_slots == 0);
    CHECK(source->count() == 5);
}

void test_partial_remove_acceptance_rejection_restores_original_state() {
    reset_fixture();
    auto& bag = runtime_state().objects.bag;
    auto& objects = qol::extension_bag::ui_state().objects;
    constexpr int kBag = 0;
    constexpr int kSlot = 1;
    constexpr int kCategory = 77;
    int item_object = 2;
    bag.items[kBag][kSlot] = make_item(kCategory, 3);
    objects.module_objects[kBag][kSlot] = &item_object;
    objects.module_object_categories[kBag][kSlot] = kCategory;
    objects.module_object_hashes[kBag][kSlot] =
        virtual_bag::payload_hash(bag.items[kBag][kSlot]);
    g_handle_item = &item_object;
    g_effective_item = &item_object;
    g_effective_count = 3;
    objects.module_object_use[kBag][kSlot].generation = 31;

    uint32_t internal_handle = 0;
    CHECK(ownership::allocate(&objects.ownership_ledger, &internal_handle) ==
          ownership::Outcome::kOk);
    objects.module_object_handles[kBag][kSlot] = internal_handle;

    ModuleUseToken token{};
    CHECK(module_use_begin_locked(kBag, kSlot, &item_object, &token));
    g_active_module_use_token = &token;
    const virtual_bag::Item descriptor_before = bag.items[kBag][kSlot];
    const uint32_t hash_before = objects.module_object_hashes[kBag][kSlot];
    const int category_before = objects.module_object_categories[kBag][kSlot];
    const void* object_before = objects.module_objects[kBag][kSlot];
    const uint32_t handle_before = objects.module_object_handles[kBag][kSlot];
    const ModuleUseState use_before = objects.module_object_use[kBag][kSlot];
    const ownership::Ledger ledger_before = objects.ownership_ledger;
    const qol::contracts::QolItemHandle public_handle_before = g_public_handle;

    // This one-shot false models only rejection of the in-memory mutation acceptance
    // seam. It does not model slot-data persistence or a durable/save failure.
    g_reject_next_persist = true;
    qol::contracts::QolBagMutationResult mutation{};
    const qol::contracts::QolBagSource* source =
        qol::features::extension_bag::extension_bag_bag_source();
    const QolResult result = source->remove(g_public_handle, 1, &mutation);

    CHECK(!qol::contracts::succeeded(result));
    CHECK(result.code == kQolErrInternal);
    CHECK(mutation.ref.space_id == 0 && mutation.ref.bag == 0 &&
          mutation.item.slot == 0 && mutation.item.generation == 0 && mutation.count == 0);
    CHECK(g_persist_calls == 1 && !g_reject_next_persist);
    CHECK(bag.items[kBag][kSlot].category == descriptor_before.category);
    CHECK(bag.items[kBag][kSlot].count == descriptor_before.count);
    CHECK(bag.items[kBag][kSlot].payload_size == descriptor_before.payload_size);
    CHECK(bag.items[kBag][kSlot].payload == descriptor_before.payload);
    CHECK(virtual_bag::payload_count(bag.items[kBag][kSlot]) == 3);
    CHECK(objects.module_object_hashes[kBag][kSlot] == hash_before);
    CHECK(hash_before == virtual_bag::payload_hash(descriptor_before));
    CHECK(objects.module_object_categories[kBag][kSlot] == category_before);
    CHECK(objects.module_objects[kBag][kSlot] == object_before);
    CHECK(objects.module_object_handles[kBag][kSlot] == handle_before);
    CHECK(g_effective_count == 3);
    CHECK(objects.module_object_use[kBag][kSlot].generation == use_before.generation);
    CHECK(objects.module_object_use[kBag][kSlot].active);
    CHECK(!objects.module_object_use[kBag][kSlot].consumed);
    CHECK(!objects.module_object_use[kBag][kSlot].pending_release);
    CHECK(objects.module_object_use[kBag][kSlot].item == use_before.item);
    CHECK(objects.module_object_use[kBag][kSlot].owner == use_before.owner);
    CHECK(g_active_module_use_token == &token);
    CHECK(token.bag == kBag && token.slot == kSlot && token.item == &item_object);
    CHECK(token.generation == use_before.generation && token.owner == use_before.owner);
    CHECK(same_ledger(objects.ownership_ledger, ledger_before));
    CHECK(ledger_has_single_module_object(objects.ownership_ledger, internal_handle));
    CHECK(g_extension_free_calls == 0);

    qol::contracts::QolInventoryItemRef ref{};
    const QolResult read_result = source->item_at(kBag, kSlot, &ref);
    CHECK(qol::contracts::succeeded(read_result));
    CHECK(ref.bag == kBag && ref.slot == kSlot && ref.category == kCategory && ref.count == 3);
    CHECK(ref.item.slot == public_handle_before.slot &&
          ref.item.generation == public_handle_before.generation);
    CHECK(g_effective_count == 3);
    CHECK(g_extension_free_calls == 0);

    g_active_module_use_token = nullptr;
    CHECK(module_use_abort_locked(token));
    CHECK(!objects.module_object_use[kBag][kSlot].active);
}

}  // namespace

// Test-only host ports. The callbacks, module_item_locked, ownership checks, generation updates,
// and free_module_object_locked implementation are production code. Only unavailable game/Host
// edges are supplied here; the materializer deliberately returns failure to exercise its real
// caller's rollback path.
bool read_world_in_world() { return true; }
bool extension_bag_session_admitted_locked() { return true; }

void extension_bag_mark_dirty_locked() {
    runtime_state().persistence.dirty = true;
}

bool persist_state_locked(bool force) {
    (void)force;
    ++g_persist_calls;
    if (g_reject_next_persist) {
        g_reject_next_persist = false;
        return false;
    }
    // Test-only acceptance model: true mirrors production; the one-shot false above is not a
    // durable/save failure and does not model the slot-data persistence transaction.
    return true;
}

const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api() {
    return nullptr;
}

uint32_t virtual_bag_effective_count(const void* item, uint32_t fallback_canonical) {
    if (item == g_effective_item) return g_effective_count;
    return fallback_canonical;
}

bool virtual_bag_write_effective(void* item, uint32_t count) {
    if (item == g_effective_item) g_effective_count = count;
    return true;
}

uint32_t virtual_bag_count_limit(bool extended) {
    (void)extended;
    return 999;
}

qol::contracts::QolItemCountEncoding virtual_bag_category_uses_stack_count(int category) {
    return category == 77 ? qol::contracts::QolItemCountEncoding::kEncoded
                          : qol::contracts::QolItemCountEncoding::kNotEncoded;
}

bool extension_bag_register_item_handle(void* item) { return item != nullptr; }

bool extension_bag_item_handle_for_pointer(
    void* item, qol::contracts::QolItemHandle* out_handle) {
    if (item == nullptr || item != g_handle_item || out_handle == nullptr) return false;
    *out_handle = g_public_handle;
    return true;
}

bool extension_bag_private_item_for_handle(qol::contracts::QolItemHandle handle,
                                           void** out_item) {
    if (out_item == nullptr || g_handle_item == nullptr ||
        handle.slot != g_public_handle.slot ||
        handle.generation != g_public_handle.generation) {
        return false;
    }
    *out_item = g_handle_item;
    return true;
}

bool extension_bag_rebind_item_handle(void* item) { return item != nullptr; }

static bool g_game_owned_item = false;

bool extension_bag_item_owned_by_game(void* item) {
    (void)item;
    return g_game_owned_item;
}

void extension_bag_invalidate_item_handle(void* item) {
    if (item == g_handle_item) g_handle_item = nullptr;
}

bool extension_bag_sale_item_matches(qol::contracts::QolItemHandle expected, void* item,
                                     qol::contracts::QolItemHandle* out_current) {
    (void)expected;
    (void)item;
    (void)out_current;
    return false;
}

bool extension_bag_sale_item_begin_release(qol::contracts::QolItemHandle expected,
                                           void* item) {
    (void)expected;
    (void)item;
    return false;
}

bool extension_bag_sale_item_finish_release(qol::contracts::QolItemHandle expected,
                                            bool committed) {
    (void)expected;
    (void)committed;
    return false;
}

bool extension_free_item(void* item) {
    (void)item;
    ++g_extension_free_calls;
    return true;
}

namespace qol::extension_bag {

bool extension_bag_on_main_thread() { return true; }

}  // namespace qol::extension_bag

std::string extension_bag_api_use_item_impl(int logical_bag, int slot) {
    (void)logical_bag;
    (void)slot;
    return "{\"ok\":false}";
}

void* materialize_module_item_locked(const virtual_bag::Item& descriptor) {
    (void)descriptor;
    ++g_materialize_calls;
    return nullptr;
}

void release_temporary_item_locked(void* item) { (void)item; }

void* valid_child_locked(void* root, int slot) {
    (void)root;
    (void)slot;
    return nullptr;
}

bool extension_bag_ui_touch_state(void** out_moving, void** out_drop_source,
                                  int64_t* out_x, int64_t* out_y) {
    if (out_moving != nullptr) *out_moving = nullptr;
    if (out_drop_source != nullptr) *out_drop_source = nullptr;
    if (out_x != nullptr) *out_x = 0;
    if (out_y != nullptr) *out_y = 0;
    return false;
}

bool extension_bag_ui_set_control_item(void* control, void* item) {
    (void)control;
    (void)item;
    return false;
}

// Unused projection/UI edges retained because refresh_module_item_area_locked is
// production-linked from the callback table. The mutation cases keep the module view
// closed, so these adapters only satisfy unrelated refresh/install sections.
thread_local bool g_in_native_call = false;
thread_local bool g_refresh_restore_suppress = false;

namespace qol::features::extension_bag {

BagSizeProjectionScope::BagSizeProjectionScope() = default;
BagSizeProjectionScope::~BagSizeProjectionScope() = default;
DisplaySubstitutionSuppressScope::DisplaySubstitutionSuppressScope() = default;
DisplaySubstitutionSuppressScope::~DisplaySubstitutionSuppressScope() = default;

void extension_bag_display_begin_pending(qol::contracts::QolUiRootKind kind, int bag,
                                         int window_bag) {
    (void)kind;
    (void)bag;
    (void)window_bag;
}

void extension_bag_display_commit() {}
void extension_bag_display_cancel() {}

}  // namespace qol::features::extension_bag

void restore_extension_desc_popup_callbacks_locked() {}
void reset_extension_desc_unequip_hook_locked() {}
void reset_extension_desc_item_hooks_locked() {}

int projection_proxy_bag_locked() { return 0; }
int raw_current_bag_locked() { return 0; }
void set_original_bag_locked(int bag) { (void)bag; }
bool bag_size_word_locked(int bag, uint32_t* out_word) {
    (void)bag;
    if (out_word == nullptr) return false;
    *out_word = 1;
    return true;
}

void* extension_bag_ui_root(qol::contracts::QolUiRootKind kind) {
    (void)kind;
    return nullptr;
}

bool extension_bag_ui_child_count(void* root, int* out_count) {
    (void)root;
    if (out_count != nullptr) *out_count = 0;
    return false;
}

void* extension_bag_ui_data(void* control) {
    (void)control;
    return nullptr;
}

bool extension_bag_ui_set_control_show(void* control, bool show) {
    (void)control;
    (void)show;
    return false;
}

void* extension_bag_ui_child_at(void* root, int slot) {
    (void)root;
    (void)slot;
    return nullptr;
}

void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param,
                           uint64_t param2, int64_t x, int64_t y) {
    (void)phase;
    (void)event;
    (void)param;
    (void)param2;
    (void)x;
    (void)y;
}

void test_deferred_free_skips_game_owned_and_clears_on_load() {
    int dummy = 0;
    clear_deferred_free_queue_locked();
    const size_t before = deferred_free_pending_count_locked();
    g_game_owned_item = true;
    CHECK(defer_item_free_locked(&dummy));
    CHECK(deferred_free_pending_count_locked() == before);
    g_game_owned_item = false;
    CHECK(defer_item_free_locked(&dummy));
    CHECK(deferred_free_pending_count_locked() == before + 1);
    clear_deferred_free_queue_locked();
    CHECK(deferred_free_pending_count_locked() == 0);
}

int main() {
    test_free_rejection_preserves_slot_and_active_use_custody();
    test_materialize_rejection_restores_empty_descriptor_and_handle_state();
    test_partial_remove_acceptance_rejection_restores_original_state();
    test_deferred_free_skips_game_owned_and_clears_on_load();
    std::printf("extension_bag_bag_source_mutation: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
