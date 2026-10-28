#include "plugins/core.item/runtime/core_item_internal.h"
#include "plugins/core.item/usecases/move_item.h"
#include "plugins/core.item/usecases/transfer_journal.h"
#include "contracts/save_facts.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace transfer_journal = qol::features::core_item::usecases::transfer_journal;

namespace {

using namespace qol;
using namespace qol::contracts;
using namespace qol::features::core_item;
using namespace qol::features::core_item::usecases;

constexpr int32_t kFakeSpace = contracts::kQolBagSpaceFirstRegistered;
constexpr int32_t kStoreSlotCount = 2;
constexpr uint32_t kStoredBytesCapacity = 1024;

struct StoredSection {
    bool present = false;
    uint16_t version = 0;
    uint32_t size = 0;
    std::array<uint8_t, kStoredBytesCapacity> bytes{};
};

struct FakeItem {
    bool present = false;
    int32_t category = 0;
    int32_t count = 0;
    QolItemHandle handle{};
};

enum class TraceKind { kStoreRead, kStoreWrite, kStoreRemove, kBagRemove };

struct TraceEntry {
    TraceKind kind;
    int32_t slot_or_bag;
    bool succeeded;
};

std::array<StoredSection, kStoreSlotCount> g_store{};
std::array<std::array<FakeItem, 4>, 2> g_items{};
std::vector<TraceEntry> g_trace;
int32_t g_current_slot = 0;
uint32_t g_write_calls = 0;
uint32_t g_fail_write_call = 0;
uint32_t g_next_item_id = 100;
bool g_fail_source_remove = false;

QolResult failure(int32_t code) {
    static constexpr char kMessage[] = "injected test failure";
    return contracts::error(code, kMessage, sizeof(kMessage) - 1);
}

QolResult read_current_slot(int32_t* out_slot) {
    if (out_slot == nullptr) return failure(kQolErrInvalidArgument);
    *out_slot = g_current_slot;
    return contracts::ok();
}

const contracts::SaveFactsApi kSaveFactsApi{
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    &read_current_slot, nullptr, nullptr};

QolResult require_capability(const char* id, QolVersionRange range, const void** out_iface) {
    if (id == nullptr || out_iface == nullptr ||
        std::strcmp(id, contracts::kSaveFactsCapabilityId) != 0 ||
        range.major != contracts::kSaveFactsVersionMajor ||
        range.min_minor > contracts::kSaveFactsVersionMinor) {
        return failure(kQolErrUnsupported);
    }
    *out_iface = &kSaveFactsApi;
    return contracts::ok();
}

QolResult store_read(int32_t slot, const char*, QolStoreBuffer* out,
                     uint16_t* out_version) {
    g_trace.push_back({TraceKind::kStoreRead, slot, true});
    if (slot < 0 || slot >= kStoreSlotCount || out == nullptr || out_version == nullptr) {
        return failure(kQolErrInvalidArgument);
    }
    const StoredSection& section = g_store[static_cast<std::size_t>(slot)];
    if (!section.present) return failure(kQolErrNotFound);
    if (out->data == nullptr || out->capacity < section.size) {
        return failure(kQolErrInvalidArgument);
    }
    std::memcpy(out->data, section.bytes.data(), section.size);
    out->size = section.size;
    *out_version = section.version;
    return contracts::ok();
}

QolResult store_write(int32_t slot, const char*, uint16_t version, const uint8_t* bytes,
                      uint32_t size) {
    ++g_write_calls;
    const bool injected_failure = g_write_calls == g_fail_write_call;
    g_trace.push_back({TraceKind::kStoreWrite, slot, !injected_failure});
    if (injected_failure) return failure(kQolErrInternal);
    if (slot < 0 || slot >= kStoreSlotCount || bytes == nullptr ||
        size > kStoredBytesCapacity) {
        return failure(kQolErrInvalidArgument);
    }
    StoredSection& section = g_store[static_cast<std::size_t>(slot)];
    section.present = true;
    section.version = version;
    section.size = size;
    std::memcpy(section.bytes.data(), bytes, size);
    return contracts::ok();
}

QolResult store_remove(int32_t slot, const char*) {
    g_trace.push_back({TraceKind::kStoreRemove, slot, true});
    if (slot < 0 || slot >= kStoreSlotCount) return failure(kQolErrInvalidArgument);
    StoredSection& section = g_store[static_cast<std::size_t>(slot)];
    if (!section.present) return failure(kQolErrNotFound);
    section = StoredSection{};
    return contracts::ok();
}

contracts::QolHostSdk make_fake_sdk() {
    contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(contracts::QolHostSdk), contracts::kHostSdkMajor,
               contracts::kHostSdkMinor};
    sdk.capabilities.require = &require_capability;
    sdk.store.read = &store_read;
    sdk.store.write = &store_write;
    sdk.store.remove = &store_remove;
    return sdk;
}

FakeItem* item_at(int32_t bag, int32_t slot) {
    if (bag < 0 || bag >= static_cast<int32_t>(g_items.size()) || slot < 0 ||
        slot >= static_cast<int32_t>(g_items[0].size())) {
        return nullptr;
    }
    return &g_items[static_cast<std::size_t>(bag)][static_cast<std::size_t>(slot)];
}

FakeItem* find_item(QolItemHandle handle, int32_t* out_bag, int32_t* out_slot) {
    for (int32_t bag = 0; bag < static_cast<int32_t>(g_items.size()); ++bag) {
        for (int32_t slot = 0; slot < static_cast<int32_t>(g_items[0].size()); ++slot) {
            FakeItem& candidate = g_items[static_cast<std::size_t>(bag)]
                                           [static_cast<std::size_t>(slot)];
            if (candidate.present && candidate.handle.slot == handle.slot &&
                candidate.handle.generation == handle.generation) {
                if (out_bag != nullptr) *out_bag = bag;
                if (out_slot != nullptr) *out_slot = slot;
                return &candidate;
            }
        }
    }
    return nullptr;
}

QolResult bag_count() { return contracts::ok(); }

QolResult bag_info(int32_t bag, contracts::QolBagInfo* out) {
    if (out == nullptr || bag < 0 || bag >= static_cast<int32_t>(g_items.size())) {
        return failure(kQolErrInvalidArgument);
    }
    int32_t occupied = 0;
    for (const FakeItem& item : g_items[static_cast<std::size_t>(bag)]) {
        if (item.present) ++occupied;
    }
    *out = {static_cast<int32_t>(g_items[0].size()), occupied};
    return contracts::ok();
}

QolResult bag_item_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (out == nullptr) return failure(kQolErrInvalidArgument);
    FakeItem* item = item_at(bag, slot);
    if (item == nullptr) return failure(kQolErrInvalidArgument);
    if (!item->present) return failure(kQolErrNotFound);
    *out = {static_cast<int32_t>(contracts::QolInventoryItemKind::kProvider), bag, slot,
            item->category, item->count, item->handle, kFakeSpace};
    return contracts::ok();
}

QolResult bag_export(QolItemHandle handle, int32_t count, QolBagTransfer* out) {
    if (out == nullptr || count <= 0) return failure(kQolErrInvalidArgument);
    FakeItem* item = find_item(handle, nullptr, nullptr);
    if (item == nullptr || count > item->count) return failure(kQolErrNotFound);
    *out = {};
    out->category = item->category;
    out->count = count;
    out->payload_size = sizeof(uint32_t);
    std::memcpy(out->payload, &handle.slot, sizeof(handle.slot));
    return contracts::ok();
}

QolResult bag_import(const QolBagTransfer* transfer, int32_t bag, int32_t slot,
                     QolBagMutationResult* out) {
    if (transfer == nullptr || out == nullptr || transfer->count <= 0) {
        return failure(kQolErrInvalidArgument);
    }
    FakeItem* item = item_at(bag, slot);
    if (item == nullptr || item->present) return failure(kQolErrConflict);
    item->present = true;
    item->category = transfer->category;
    item->count = transfer->count;
    item->handle = {g_next_item_id++, 1};
    *out = {{kFakeSpace, bag}, item->handle, transfer->count};
    return contracts::ok();
}

QolResult bag_remove(QolItemHandle handle, int32_t count, QolBagMutationResult* out) {
    if (count <= 0 || out == nullptr) return failure(kQolErrInvalidArgument);
    int32_t bag = -1;
    int32_t slot = -1;
    FakeItem* item = find_item(handle, &bag, &slot);
    if (item == nullptr || item->count != count) return failure(kQolErrNotFound);
    if (bag == 0 && g_fail_source_remove) {
        g_trace.push_back({TraceKind::kBagRemove, bag, false});
        g_fail_source_remove = false;
        return failure(kQolErrBlocked);
    }
    g_trace.push_back({TraceKind::kBagRemove, bag, true});
    item->present = false;
    *out = {{kFakeSpace, bag}, handle, count};
    return contracts::ok();
}

contracts::QolBagSource make_fake_source() {
    contracts::QolBagSource source{};
    source.version = contracts::kBagSourceVersion;
    source.space_id = kFakeSpace;
    source.count = +[] { return static_cast<int32_t>(2); };
    source.info = &bag_info;
    source.item_at = &bag_item_at;
    source.export_item = &bag_export;
    source.import_item = &bag_import;
    source.remove = &bag_remove;
    return source;
}

void put_item(int32_t bag, int32_t slot, int32_t category, int32_t count,
              uint32_t handle_slot) {
    FakeItem* item = item_at(bag, slot);
    assert(item != nullptr);
    *item = {true, category, count, {handle_slot, 1}};
}

bool item_present(int32_t bag, int32_t slot) {
    const FakeItem* item = item_at(bag, slot);
    return item != nullptr && item->present;
}

std::size_t trace_index(TraceKind kind, int32_t slot_or_bag, bool succeeded) {
    for (std::size_t index = 0; index < g_trace.size(); ++index) {
        const TraceEntry& entry = g_trace[index];
        if (entry.kind == kind && entry.slot_or_bag == slot_or_bag &&
            entry.succeeded == succeeded) {
            return index;
        }
    }
    return g_trace.size();
}

}  // namespace

namespace qol::features::core_item {

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolItemPort* g_port = nullptr;
const contracts::QolItemInventoryStateWritePort* g_item_state_write = nullptr;

const contracts::QolBagSource* find_bag_source(int32_t space_id) {
    static const contracts::QolBagSource source = make_fake_source();
    return space_id == source.space_id ? &source : nullptr;
}

void request_inventory_refresh() {}

}  // namespace qol::features::core_item

int main() {
    static contracts::QolHostSdk sdk = make_fake_sdk();
    qol::features::core_item::g_sdk = &sdk;

    const contracts::QolBagRef source_ref{kFakeSpace, 0};
    const contracts::QolBagRef target_ref{kFakeSpace, 1};
    transfer_journal::JournalId journal_id{};

    assert(transfer_journal::begin(source_ref, 0, target_ref, 0, 12, 1, &journal_id));
    assert(transfer_journal::count() == 1);
    assert(g_write_calls == 1);
    assert(g_store[0].present);
    const uint32_t persisted_size = g_store[0].size;
    const std::array<uint8_t, kStoredBytesCapacity> persisted_intent = g_store[0].bytes;

    g_fail_write_call = 2;
    transfer_journal::mark_target_written(journal_id, 0x12345678u);
    assert(g_write_calls == 2);
    assert(g_store[0].present);
    assert(g_store[0].size == persisted_size);
    assert(std::memcmp(g_store[0].bytes.data(), persisted_intent.data(), persisted_size) == 0);

    // Slot changes force the public journal API to reload slot 0 from persistent storage.
    put_item(0, 0, 12, 1, 10);
    put_item(1, 0, 12, 1, 11);
    g_current_slot = 1;
    assert(transfer_journal::reconcile());
    assert(transfer_journal::count() == 0);
    g_current_slot = 0;
    assert(transfer_journal::reconcile());
    assert(transfer_journal::count() == 0);
    assert(item_present(0, 0));
    assert(!item_present(1, 0));
    assert(!g_store[0].present);
    assert(trace_index(TraceKind::kStoreRead, 0, true) < g_trace.size());
    assert(trace_index(TraceKind::kStoreRemove, 0, true) < g_trace.size());

    g_trace.clear();
    g_fail_write_call = 0;
    g_fail_source_remove = true;
    g_items = {};
    put_item(0, 0, 12, 1, 20);
    contracts::QolBagMutationResult target_result{};
    const QolResult moved = move_item(source_ref, 0, 1, target_ref, 0, &target_result);
    assert(!contracts::succeeded(moved));
    assert(moved.code == kQolErrBlocked);
    assert(transfer_journal::count() == 0);
    assert(item_present(0, 0));
    assert(!item_present(1, 0));
    assert(!g_store[0].present);

    const std::size_t failed_source_remove =
        trace_index(TraceKind::kBagRemove, 0, false);
    const std::size_t compensated_target_remove =
        trace_index(TraceKind::kBagRemove, 1, true);
    const std::size_t journal_remove = trace_index(TraceKind::kStoreRemove, 0, true);
    assert(failed_source_remove < compensated_target_remove);
    assert(compensated_target_remove < journal_remove);

    // The marker write is best-effort (void API); when both item operations succeed,
    // verify the completed transfer's actual terminal state without requiring the
    // marker failure itself to stop source removal.
    g_trace.clear();
    g_items = {};
    put_item(0, 0, 12, 1, 30);
    g_write_calls = 0;
    g_fail_write_call = 2;
    g_fail_source_remove = false;
    const QolResult completed_move =
        move_item(source_ref, 0, 1, target_ref, 0, &target_result);
    const bool begin_write_succeeded =
        trace_index(TraceKind::kStoreWrite, 0, true) < g_trace.size();
    const bool target_mark_write_failed_on_second_call =
        trace_index(TraceKind::kStoreWrite, 0, false) < g_trace.size() &&
        g_write_calls == 2;
    const bool completed_transfer_state = contracts::succeeded(completed_move) &&
                                          !item_present(0, 0) && item_present(1, 0) &&
                                          transfer_journal::count() == 0 && !g_store[0].present;
    assert(begin_write_succeeded);
    assert(target_mark_write_failed_on_second_call);
    assert(completed_transfer_state);
    return 0;
}
