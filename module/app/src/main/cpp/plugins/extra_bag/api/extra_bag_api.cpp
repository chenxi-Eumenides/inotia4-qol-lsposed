#include "extra_bag_api.h"

#include <cstring>
#include <optional>

#include "plugins/extra_bag/model/store.h"

namespace qol::features::extra_bag::api {
namespace {

using model::Bag;
using model::ItemBackpack;
using model::RuntimeItem;
using model::store;

contracts::QolExtraBagItem to_transfer(const RuntimeItem& item) {
    contracts::QolExtraBagItem out{};
    out.category = item.category;
    out.count = item.count;
    out.payload_size = item.payload_size;
    std::memcpy(out.payload, item.payload, sizeof(out.payload));
    return out;
}

RuntimeItem to_runtime(const contracts::QolExtraBagItem& item) {
    RuntimeItem out{};
    out.category = item.category;
    out.count = item.count;
    out.payload_size = item.payload_size;
    std::memcpy(out.payload, item.payload, sizeof(out.payload));
    return out;
}

int32_t api_bag_count() {
    return static_cast<int32_t>(store().bag_count());
}

void api_bag_info(int32_t bag_index, contracts::QolExtraBagInfo* out) {
    if (out == nullptr) return;
    *out = {};
    const Bag* bag = store().bag(static_cast<uint32_t>(bag_index));
    if (bag == nullptr) return;
    out->capacity = static_cast<int32_t>(bag->capacity());
    out->occupied_slots = static_cast<int32_t>(bag->occupied_slots());
}

bool api_item_at(int32_t bag_index, int32_t slot_index,
                 contracts::QolExtraBagItem* out) {
    if (out == nullptr) return false;
    const RuntimeItem* item =
        store().item(static_cast<uint32_t>(bag_index), static_cast<uint32_t>(slot_index));
    if (item == nullptr) return false;
    *out = to_transfer(*item);
    return true;
}

void api_put(int32_t bag_index, int32_t slot_index,
             const contracts::QolExtraBagItem* item) {
    if (item == nullptr) return;
    store().put(static_cast<uint32_t>(bag_index), static_cast<uint32_t>(slot_index),
                to_runtime(*item));
}

bool api_take(int32_t bag_index, int32_t slot_index,
              contracts::QolExtraBagItem* out) {
    if (out == nullptr) return false;
    const std::optional<RuntimeItem> item =
        store().take(static_cast<uint32_t>(bag_index), static_cast<uint32_t>(slot_index));
    if (!item.has_value()) return false;
    *out = to_transfer(*item);
    return true;
}

bool api_item_backpack_at(int32_t bag_index, contracts::QolExtraBagItem* out) {
    if (out == nullptr) return false;
    *out = {};
    const Bag* bag = store().bag(static_cast<uint32_t>(bag_index));
    if (bag == nullptr) return false;
    const ItemBackpack* equipped = bag->item_backpack();
    if (equipped == nullptr) return false;
    *out = to_transfer(equipped->item);
    return true;
}

bool api_set_item_backpack(int32_t bag_index,
                           const contracts::QolExtraBagItem* item) {
    if (item == nullptr) return false;
    Bag* bag = store().bag(static_cast<uint32_t>(bag_index));
    if (bag == nullptr) return false;
    return bag->equip_item_backpack(ItemBackpack{to_runtime(*item)});
}

void api_clear_item_backpack(int32_t bag_index) {
    Bag* bag = store().bag(static_cast<uint32_t>(bag_index));
    if (bag == nullptr) return;
    bag->unequip_item_backpack();
}

const contracts::QolExtraBagApi kApi{
    sizeof(contracts::QolExtraBagApi),
    &api_bag_count,
    &api_bag_info,
    &api_item_at,
    &api_put,
    &api_take,
    &api_item_backpack_at,
    &api_set_item_backpack,
    &api_clear_item_backpack,
};

}  // namespace

const contracts::QolExtraBagApi& extra_bag_api() {
    return kApi;
}

}  // namespace qol::features::extra_bag::api
