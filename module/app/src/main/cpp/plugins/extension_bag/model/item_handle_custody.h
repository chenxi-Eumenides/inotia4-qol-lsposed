#pragma once

#include <array>
#include <cstddef>

#include "contracts/item_handle.h"

namespace qol::features::extension_bag::model {

struct DeferredFreeDrainResult {
    size_t kept = 0;
    size_t freed = 0;
    size_t unprocessed = 0;
};

template <typename Entry, size_t Capacity, typename CanFree, typename FreeEntry>
DeferredFreeDrainResult drain_deferred_free_entries(std::array<Entry, Capacity>& entries,
                                                      size_t head, size_t count,
                                                      CanFree can_free,
                                                      FreeEntry free_entry) {
    static_assert(Capacity > 0, "deferred free queue capacity must be positive");
    DeferredFreeDrainResult result{};
    const size_t bounded_count = count < Capacity ? count : Capacity;
    result.unprocessed = count - bounded_count;

    for (size_t offset = 0; offset < bounded_count; ++offset) {
        const size_t source = (head + offset) % Capacity;
        const Entry entry = entries[source];
        if (can_free(entry) && free_entry(entry)) {
            ++result.freed;
            continue;
        }
        if (result.kept >= Capacity) {
            // A valid bounded pass cannot reach this branch; keep the source entry untouched.
            ++result.unprocessed;
            continue;
        }
        const size_t destination = (head + result.kept) % Capacity;
        entries[destination] = entry;
        ++result.kept;
    }

    for (size_t offset = result.kept; offset < bounded_count; ++offset) {
        entries[(head + offset) % Capacity] = Entry{};
    }
    return result;
}

inline int find_handle_slot(contracts::QolItemHandle expected,
                            const contracts::QolItemHandle* slots, size_t slot_count) {
    if (expected.slot == 0 || expected.generation == 0 || slots == nullptr) return -1;
    for (size_t slot = 0; slot < slot_count; ++slot) {
        if (slots[slot].slot == expected.slot &&
            slots[slot].generation == expected.generation) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

class ItemHandleCustody {
public:
    static constexpr size_t kCapacity = 1024;

    enum class State : uint8_t { kEmpty, kActive, kReleasing };

    bool bind(void* item, contracts::QolItemHandle handle) {
        if (item == nullptr || !valid(handle)) return false;
        Entry* free_entry = nullptr;
        for (Entry& entry : entries_) {
            if (entry.state != State::kEmpty && entry.item == item) {
                if (entry.state == State::kReleasing) return false;
                if (same_handle(entry.handle, handle)) return true;
                entry = {};
            }
            if (free_entry == nullptr && entry.state == State::kEmpty) free_entry = &entry;
        }
        if (free_entry == nullptr) return false;
        *free_entry = {item, handle, State::kActive};
        return true;
    }

    bool lookup(contracts::QolItemHandle handle, void** out_item) const {
        if (out_item == nullptr || !valid(handle)) return false;
        *out_item = nullptr;
        for (const Entry& entry : entries_) {
            if (entry.state == State::kActive && same_handle(entry.handle, handle)) {
                *out_item = entry.item;
                return entry.item != nullptr;
            }
        }
        return false;
    }

    bool begin_release(contracts::QolItemHandle handle) {
        Entry* entry = entry_for(handle);
        if (entry == nullptr || entry->state != State::kActive) return false;
        entry->state = State::kReleasing;
        return true;
    }

    bool finish_release(contracts::QolItemHandle handle, bool committed) {
        Entry* entry = entry_for(handle);
        if (entry == nullptr || entry->state != State::kReleasing) return false;
        if (committed) *entry = {};
        else entry->state = State::kActive;
        return true;
    }

    bool rebind(void* item, contracts::QolItemHandle previous,
                contracts::QolItemHandle current) {
        if (item == nullptr || !valid(previous) || !valid(current)) return false;
        Entry* old_entry = entry_for(previous);
        if (old_entry != nullptr && old_entry->item != item) return false;
        if (old_entry != nullptr && old_entry->state == State::kReleasing) return false;
        if (old_entry != nullptr) *old_entry = {};
        return bind(item, current);
    }

    void invalidate_all() {
        entries_ = {};
    }

private:
    struct Entry {
        void* item = nullptr;
        contracts::QolItemHandle handle{};
        State state = State::kEmpty;
    };

    static bool valid(contracts::QolItemHandle handle) {
        return handle.slot != 0 && handle.generation != 0;
    }

    static bool same_handle(contracts::QolItemHandle left, contracts::QolItemHandle right) {
        return left.slot == right.slot && left.generation == right.generation;
    }

    Entry* entry_for(contracts::QolItemHandle handle) {
        for (Entry& entry : entries_) {
            if (entry.state != State::kEmpty && same_handle(entry.handle, handle)) return &entry;
        }
        return nullptr;
    }

    std::array<Entry, kCapacity> entries_{};
};

}  // namespace qol::features::extension_bag::model
