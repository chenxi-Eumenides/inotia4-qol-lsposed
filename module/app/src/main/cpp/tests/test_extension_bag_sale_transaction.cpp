#include "plugins/extension_bag/model/sale_transaction.h"

#include <array>
#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using namespace qol::contracts;
using qol::features::extension_bag::model::SaleRemovalOps;
using qol::features::extension_bag::model::SaleRemovalResult;
using qol::features::extension_bag::model::SaleSlotState;

struct FakeRelease {
    void* expected_object = nullptr;
    QolItemHandle expected_handle{};
    bool object_live = true;
    bool handle_live = true;
    bool custody_releasing = false;
    bool fail_release = false;
    bool invalidate_reservation_after_free = false;
    ownership::Ledger* ledger = nullptr;
    uint32_t ledger_handle = 0;
    int calls = 0;
};

struct FakeProjection {
    void* expected_object = nullptr;
    bool visible = true;
    bool fail_clear = false;
    int clear_calls = 0;
    int restore_calls = 0;
};

struct CallbackContext {
    FakeProjection* projection;
    FakeRelease* release;
};

bool clear_projection(void* raw, void* control, void* item) {
    auto* context = static_cast<CallbackContext*>(raw);
    auto* projection = context->projection;
    if (control == nullptr || control != projection || item != projection->expected_object) return false;
    ++projection->clear_calls;
    if (projection->fail_clear) return false;
    projection->visible = false;
    return true;
}

bool restore_projection(void* raw, void* control, void* item) {
    auto* context = static_cast<CallbackContext*>(raw);
    auto* projection = context->projection;
    if (control == nullptr || control != projection || item != projection->expected_object) return false;
    ++projection->restore_calls;
    projection->visible = true;
    return true;
}

bool release_object(void* raw, void* item, QolItemHandle handle) {
    auto* context = static_cast<CallbackContext*>(raw);
    auto* release = context->release;
    ++release->calls;
    if (item != release->expected_object || handle.slot != release->expected_handle.slot ||
        handle.generation != release->expected_handle.generation ||
        !ownership::live_state(*release->ledger, release->ledger_handle,
                               ownership::State::kReservedForRelease) ||
        !release->handle_live || release->custody_releasing) return false;
    release->custody_releasing = true;
    if (release->fail_release) {
        release->custody_releasing = false;
        return false;
    }
    release->object_live = false;
    release->handle_live = false;
    release->custody_releasing = false;
    if (release->invalidate_reservation_after_free) {
        release->ledger->slots[ownership::handle_slot(release->ledger_handle)].state =
            ownership::State::kModuleOwned;
    }
    return true;
}

struct Harness {
    virtual_bag::State state{};
    ownership::Ledger ledger{};
    int object = 1;
    uint32_t internal_handle = 0;
    uint64_t object_generation = 7;
    bool use_active = false;
    bool use_consumed = false;
    bool use_pending_release = false;
    void* use_item = nullptr;
    std::thread::id use_owner{};
    bool dirty = false;
    std::atomic<uint64_t> revision{9};
    std::array<int, virtual_bag::kBagCount * virtual_bag::kSlotCount> categories{};
    std::array<uint32_t, virtual_bag::kBagCount * virtual_bag::kSlotCount> hashes{};
    std::array<void*, virtual_bag::kBagCount * virtual_bag::kSlotCount> objects{};
    std::array<uint32_t, virtual_bag::kBagCount * virtual_bag::kSlotCount> handles{};
    FakeRelease release{};
    FakeProjection projection{};
    CallbackContext callback_context{&projection, &release};
    SaleSlotState request{};
    SaleRemovalOps ops{};

    void prepare(int logical_bag, int slot, bool borrowed = false, bool projected = true) {
        const int bag = virtual_bag::extension_internal_bag(logical_bag);
        auto& descriptor = state.items[bag][slot];
        descriptor.category = 77;
        descriptor.count = 3;
        const size_t index = static_cast<size_t>(bag * virtual_bag::kSlotCount + slot);
        objects[index] = &object;
        categories[index] = descriptor.category;
        hashes[index] = virtual_bag::payload_hash(descriptor);
        ownership::allocate(&ledger, &internal_handle);
        if (borrowed) ownership::borrow_for_view(&ledger, internal_handle);
        handles[index] = internal_handle;
        release.ledger = &ledger;
        release.ledger_handle = internal_handle;
        const QolItemHandle item{41, 19};
        release.expected_object = &object;
        release.expected_handle = item;
        projection.expected_object = &object;
        request.logical_bag = logical_bag;
        request.slot = slot;
        request.capacity = 8;
        request.session_admitted = true;
        request.descriptor_matches_object = true;
        request.descriptor_hash_matches = true;
        request.handle_custody_matches = true;
        request.expected_item = item;
        request.current_item = item;
        request.descriptor = &descriptor;
        request.object = &objects[index];
        request.object_category = &categories[index];
        request.object_hash = &hashes[index];
        request.object_handle = &handles[index];
        request.ledger_handle = internal_handle;
        request.ledger = &ledger;
        request.use_active = &use_active;
        request.use_consumed = &use_consumed;
        request.use_pending_release = &use_pending_release;
        request.use_item = &use_item;
        request.use_generation = &object_generation;
        request.use_owner = &use_owner;
        request.dirty = &dirty;
        request.revision = &revision;
        request.projection_control = projected ? &projection : nullptr;
        callback_context = {&projection, &release};
        ops = {&callback_context, &clear_projection, &restore_projection, &release_object};
    }
};

bool descriptor_empty(const virtual_bag::Item& item) {
    return item.category == 0 && item.count == 0 && item.payload_size == 0;
}

void test_successful_whole_item_removal_and_roundtrip(int logical_bag, int slot) {
    Harness h;
    h.prepare(logical_bag, slot, true);
    const auto outcome = qol::features::extension_bag::model::remove_for_sale(h.request, h.ops);
    CHECK(outcome == SaleRemovalResult::kRemoved);
    const int bag = virtual_bag::extension_internal_bag(logical_bag);
    const size_t index = static_cast<size_t>(bag * virtual_bag::kSlotCount + slot);
    CHECK(h.state.items[bag][slot].category == 0 && h.state.items[bag][slot].count == 0);
    CHECK(h.objects[index] == nullptr);
    CHECK(h.categories[index] == 0 && h.hashes[index] == 0 && h.handles[index] == 0);
    CHECK(!h.release.object_live && !h.release.handle_live);
    CHECK(!h.projection.visible && h.projection.clear_calls == 1);
    CHECK(h.dirty && h.revision.load() == 10);
    CHECK(!ownership::live_state(h.ledger, h.internal_handle, ownership::State::kModuleOwned));
    CHECK(!ownership::live_state(h.ledger, h.internal_handle, ownership::State::kBorrowedForView));
    CHECK(ownership::audit(h.ledger).balanced && h.ledger.total_released == 1);

    const std::string payload = virtual_bag::state_json(h.state);
    virtual_bag::State reloaded{};
    const bool parsed = virtual_bag::parse_state_json(payload.c_str(), &reloaded,
                                                       [](int category) { return category == 77; });
    CHECK(parsed);
    CHECK(descriptor_empty(reloaded.items[bag][slot]));
}

void test_rejections_leave_slot_and_revision_unchanged() {
    for (int fault = 0; fault < 7; ++fault) {
        Harness h;
        h.prepare(6, 2, true);
        const auto before = h.state.items[0][2];
        const uint64_t before_revision = h.revision.load();
        const uint32_t before_ledger = h.internal_handle;
        if (fault == 0) ++h.request.expected_item.generation;
        if (fault == 1) h.request.descriptor_matches_object = false;
        if (fault == 2) h.request.session_admitted = false;
        if (fault == 3) h.use_active = true;
        if (fault == 4) h.use_pending_release = true;
        if (fault == 5) h.release.fail_release = true;
        if (fault == 6) h.request.descriptor_hash_matches = false;
        const auto result = qol::features::extension_bag::model::remove_for_sale(h.request, h.ops);
        CHECK(result != SaleRemovalResult::kRemoved);
        const auto& after = h.state.items[0][2];
        CHECK(after.category == before.category && after.count == before.count &&
              after.payload_size == before.payload_size && after.payload == before.payload);
        CHECK(h.objects[2] == &h.object);
        CHECK(h.handles[2] == before_ledger);
        CHECK(h.dirty == false && h.revision.load() == before_revision);
        CHECK(h.release.object_live && h.release.handle_live);
        CHECK(ownership::live_state(h.ledger, before_ledger,
                                    ownership::State::kBorrowedForView));
        if (fault == 5) CHECK(h.projection.visible && h.projection.restore_calls == 1);
        else CHECK(h.projection.clear_calls == 0);
    }
}

void test_clear_projection_failure_does_not_release() {
    Harness h;
    h.prepare(7, 1);
    h.projection.fail_clear = true;
    const uint64_t revision = h.revision.load();
    CHECK(qol::features::extension_bag::model::remove_for_sale(h.request, h.ops) ==
          SaleRemovalResult::kProjectionFailed);
    CHECK(h.release.calls == 0);
    CHECK(h.release.object_live && h.release.handle_live);
    CHECK(h.state.items[1][1].category == 77 && h.objects[17] == &h.object);
    CHECK(!h.dirty && h.revision.load() == revision);
}

void test_replaced_slot_object_and_custody_mismatch_reject() {
    Harness h;
    h.prepare(6, 2);
    int replacement = 2;
    h.objects[2] = &replacement;
    h.request.descriptor_matches_object = false;
    CHECK(qol::features::extension_bag::model::remove_for_sale(h.request, h.ops) ==
          SaleRemovalResult::kInvalidIdentity);
    CHECK(h.release.calls == 0 && h.projection.clear_calls == 0);
    CHECK(h.objects[2] == &replacement && h.state.items[0][2].category == 77);
    CHECK(!h.dirty && h.revision.load() == 9);

    Harness custody;
    custody.prepare(6, 2);
    custody.request.handle_custody_matches = false;
    CHECK(qol::features::extension_bag::model::remove_for_sale(custody.request, custody.ops) ==
          SaleRemovalResult::kInvalidIdentity);
    CHECK(custody.release.calls == 0 && custody.projection.visible);
    CHECK(custody.objects[2] == &custody.object && !custody.dirty && custody.revision.load() == 9);
}

void test_irreversible_release_never_reports_removal_failure() {
    Harness h;
    h.prepare(6, 2, true);
    h.release.invalidate_reservation_after_free = true;
    const auto result = qol::features::extension_bag::model::remove_for_sale(h.request, h.ops);
    CHECK(result == SaleRemovalResult::kRemovedLedgerDiagnostic);
    CHECK(!h.release.object_live && !h.release.handle_live);
    CHECK(h.objects[2] == nullptr && descriptor_empty(h.state.items[0][2]));
    CHECK(h.dirty && h.revision.load() == 10);
}

void test_ext2orig_recovery_action_checks_source_and_target() {
    using virtual_bag::Ext2OrigRecoveryAction;
    CHECK(virtual_bag::ext2orig_recovery_action(true, false) ==
          Ext2OrigRecoveryAction::kRollback);
    CHECK(virtual_bag::ext2orig_recovery_action(true, true) ==
          Ext2OrigRecoveryAction::kRollbackTarget);
    CHECK(virtual_bag::ext2orig_recovery_action(false, true) ==
          Ext2OrigRecoveryAction::kComplete);
    CHECK(virtual_bag::ext2orig_recovery_action(false, false) ==
          Ext2OrigRecoveryAction::kRetry);
}

}  // namespace

int main() {
    test_successful_whole_item_removal_and_roundtrip(6, 2);
    test_successful_whole_item_removal_and_roundtrip(7, 1);
    test_rejections_leave_slot_and_revision_unchanged();
    test_clear_projection_failure_does_not_release();
    test_replaced_slot_object_and_custody_mismatch_reject();
    test_irreversible_release_never_reports_removal_failure();
    test_ext2orig_recovery_action_checks_source_and_target();
    std::printf("extension_bag_sale_transaction_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
