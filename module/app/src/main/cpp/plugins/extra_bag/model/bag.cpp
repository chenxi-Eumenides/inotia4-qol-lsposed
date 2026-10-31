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

}  // namespace qol::features::extra_bag::model
