#include "store.h"

namespace qol::features::extra_bag::model {

Store::Store() : bags_(kDefaultBagCount) {}

uint32_t Store::bag_count() const {
    return static_cast<uint32_t>(bags_.size());
}

void Store::set_bag_count(uint32_t count) {
    bags_.resize(count);
}

Bag* Store::bag(uint32_t index) {
    return index < bags_.size() ? &bags_[index] : nullptr;
}

const Bag* Store::bag(uint32_t index) const {
    return index < bags_.size() ? &bags_[index] : nullptr;
}

void Store::clear() {
    bags_.assign(kDefaultBagCount, Bag{});
}

Store& store() {
    static Store instance;
    return instance;
}

}  // namespace qol::features::extra_bag::model
