#include "store.h"

namespace qol::features::extra_bag::model {

Store::Store() : bags_(kBagCount) {}

uint32_t Store::bag_count() const {
    return static_cast<uint32_t>(bags_.size());
}

Bag* Store::bag(uint32_t bag_index) {
    return bag_index < bags_.size() ? &bags_[bag_index] : nullptr;
}

const Bag* Store::bag(uint32_t bag_index) const {
    return bag_index < bags_.size() ? &bags_[bag_index] : nullptr;
}

void Store::put(uint32_t bag_index, uint32_t slot_index, RuntimeItem item) {
    Bag* target = bag(bag_index);
    if (target == nullptr) return;
    target->put(slot_index, item);
}

std::optional<RuntimeItem> Store::take(uint32_t bag_index, uint32_t slot_index) {
    Bag* target = bag(bag_index);
    if (target == nullptr) return std::nullopt;
    return target->take(slot_index);
}

const RuntimeItem* Store::item(uint32_t bag_index, uint32_t slot_index) const {
    const Bag* target = bag(bag_index);
    return target == nullptr ? nullptr : target->item(slot_index);
}

void Store::clear() {
    bags_.assign(kBagCount, Bag{});
}

Store& store() {
    static Store instance;
    return instance;
}

}  // namespace qol::features::extra_bag::model
