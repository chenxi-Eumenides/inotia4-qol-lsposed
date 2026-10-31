#include "bag.h"

namespace qol::features::extra_bag::model {

uint32_t Bag::capacity() const {
    return item_backpack_ ? static_cast<uint32_t>(item_backpack_->capacity) : 0;
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

void Bag::put(uint32_t slot_index, ItemData item) {
    slots_[slot_index] = item;
}

std::optional<ItemData> Bag::take(uint32_t slot_index) {
    const auto it = slots_.find(slot_index);
    if (it == slots_.end()) return std::nullopt;
    const ItemData item = it->second;
    slots_.erase(it);
    return item;
}

const ItemData* Bag::item(uint32_t slot_index) const {
    const auto it = slots_.find(slot_index);
    return it == slots_.end() ? nullptr : &it->second;
}

}  // namespace qol::features::extra_bag::model
