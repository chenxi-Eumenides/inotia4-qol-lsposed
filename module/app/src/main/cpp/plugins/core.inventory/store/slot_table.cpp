#include "slot_table.h"

#include <cstddef>
#include <utility>

namespace qol::features::core_inventory::store {

MemorySlotTable::MemorySlotTable(int32_t space_id, std::vector<int32_t> capacities)
    : space_id_(space_id), capacities_(std::move(capacities)) {
    size_t total = 0;
    for (const int32_t capacity : capacities_) {
        if (capacity > 0) total += static_cast<size_t>(capacity);
    }
    slots_.resize(total);
}

int32_t MemorySlotTable::space_id() const { return space_id_; }

int32_t MemorySlotTable::bag_count() const {
    return static_cast<int32_t>(capacities_.size());
}

int32_t MemorySlotTable::capacity(int32_t bag) const {
    if (bag < 0 || static_cast<size_t>(bag) >= capacities_.size()) return 0;
    return capacities_[static_cast<size_t>(bag)];
}

bool MemorySlotTable::locate(int32_t bag, int32_t slot, size_t* out_index) const {
    if (out_index == nullptr || bag < 0 || slot < 0 ||
        static_cast<size_t>(bag) >= capacities_.size()) {
        return false;
    }
    const int32_t capacity = capacities_[static_cast<size_t>(bag)];
    if (slot >= capacity || capacity <= 0) return false;

    size_t offset = 0;
    for (int32_t index = 0; index < bag; ++index) {
        const int32_t previous = capacities_[static_cast<size_t>(index)];
        if (previous > 0) offset += static_cast<size_t>(previous);
    }
    *out_index = offset + static_cast<size_t>(slot);
    return *out_index < slots_.size();
}

bool MemorySlotTable::get(int32_t bag, int32_t slot,
                          contracts::QolInventoryItem* out) const {
    if (out == nullptr) return false;
    size_t index = 0;
    if (!locate(bag, slot, &index)) return false;
    const Slot& entry = slots_[index];
    if (!entry.occupied) return false;
    *out = entry.item;
    return true;
}

bool MemorySlotTable::set(int32_t bag, int32_t slot,
                          const contracts::QolInventoryItem& item) {
    size_t index = 0;
    if (!locate(bag, slot, &index)) return false;
    slots_[index].item = item;
    slots_[index].occupied = true;
    return true;
}

bool MemorySlotTable::clear(int32_t bag, int32_t slot) {
    size_t index = 0;
    if (!locate(bag, slot, &index)) return false;
    slots_[index] = Slot{};
    return true;
}

}  // namespace qol::features::core_inventory::store
