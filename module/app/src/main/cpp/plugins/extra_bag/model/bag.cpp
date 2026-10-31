#include "bag.h"

namespace qol::features::extra_bag::model {

uint32_t Bag::capacity() const {
    return item_backpack_ ? capacity_for_item_backpack_type(item_backpack_->type) : 0;
}

bool Bag::has_item_backpack() const {
    return item_backpack_.has_value();
}

const ItemBackpack* Bag::item_backpack() const {
    return item_backpack_ ? &*item_backpack_ : nullptr;
}

void Bag::equip_item_backpack(ItemBackpack backpack) {
    item_backpack_ = backpack;
}

void Bag::unequip_item_backpack() {
    item_backpack_.reset();
}

void Bag::put(uint32_t slot_index, RuntimeItem item) {
    slots_[slot_index] = item;
}

std::optional<RuntimeItem> Bag::take(uint32_t slot_index) {
    const auto it = slots_.find(slot_index);
    if (it == slots_.end()) return std::nullopt;
    const RuntimeItem item = it->second;
    slots_.erase(it);
    return item;
}

const RuntimeItem* Bag::item(uint32_t slot_index) const {
    const auto it = slots_.find(slot_index);
    return it == slots_.end() ? nullptr : &it->second;
}

uint32_t Bag::occupied_slots() const {
    return static_cast<uint32_t>(slots_.size());
}

}  // namespace qol::features::extra_bag::model
