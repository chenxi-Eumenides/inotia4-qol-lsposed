#include "read_only.h"

#include "plugin.h"

#include <limits>

namespace qol::features::autosell::read_only {
namespace {

const contracts::ItemFactsApi* facts() { return item_facts(); }
const contracts::QolGameHost* game() { return game_host(); }
bool read_view(contracts::QolItemHandle item, contracts::QolItemInventoryView* out) {
    const auto* api = facts();
    return api != nullptr && api->view != nullptr &&
           contracts::succeeded(api->view(item, out));
}

}  // namespace

bool item_category(contracts::QolItemHandle item, int32_t* out) {
    const auto* api = facts();
    return api != nullptr && api->category != nullptr &&
           contracts::succeeded(api->category(item, out));
}

bool item_is_jewel(int32_t category, int32_t* out) {
    const auto* api = facts();
    return api != nullptr && api->is_jewel_category != nullptr &&
           contracts::succeeded(api->is_jewel_category(category, out));
}

bool item_is_real_equip(contracts::QolItemHandle item, int32_t* out) {
    contracts::QolItemInventoryView view{};
    if (out == nullptr || !read_view(item, &view)) return false;
    *out = view.equip;
    return true;
}

bool item_rarity(contracts::QolItemHandle item, int32_t* out) {
    contracts::QolItemInventoryView view{};
    if (out == nullptr || !read_view(item, &view) ||
        (view.fields & contracts::kQolItemInventoryHasRarity) == 0) return false;
    *out = view.rarity;
    return true;
}

bool item_enchant_remaining(contracts::QolItemHandle item, int32_t* out) {
    contracts::QolItemInventoryView view{};
    if (out == nullptr || !read_view(item, &view)) return false;
    *out = view.enchant_remaining;
    return true;
}

bool item_socket_filled(contracts::QolItemHandle item, int32_t* out) {
    contracts::QolItemInventoryView view{};
    if (out == nullptr || !read_view(item, &view)) return false;
    *out = view.socket_filled;
    return true;
}

bool item_socket_total(contracts::QolItemHandle item, int32_t* out) {
    contracts::QolItemInventoryView view{};
    if (out == nullptr || !read_view(item, &view)) return false;
    *out = view.socket_total;
    return true;
}

bool item_is_backpack(int32_t category, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.is_backpack_category != nullptr &&
           contracts::succeeded(host->read_only.item.is_backpack_category(category, out));
}

bool item_is_dice(int32_t category, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.is_dice != nullptr &&
           contracts::succeeded(host->read_only.item.is_dice(category, out));
}

bool item_is_no_sell(int32_t category, int32_t* out) {
    const auto* api = facts();
    return api != nullptr && api->is_no_sell != nullptr &&
           contracts::succeeded(api->is_no_sell(category, out));
}

bool item_jewel_value(contracts::QolItemHandle item, int32_t* out) {
    contracts::QolItemInventoryView view{};
    if (out == nullptr || !read_view(item, &view) ||
        (view.fields & contracts::kQolItemInventoryHasJewelValue) == 0) return false;
    *out = view.jewel_value;
    return true;
}

bool item_jewel_attribute_type(contracts::QolItemHandle item, int32_t* out) {
    contracts::QolItemInventoryView view{};
    if (out == nullptr || !read_view(item, &view) ||
        (view.fields & contracts::kQolItemInventoryHasJewelAttributeType) == 0) return false;
    *out = view.jewel_attribute_type;
    return true;
}

bool item_inventory_view(contracts::QolItemHandle item, contracts::QolItemInventoryView* out) {
    return read_view(item, out);
}

bool world_state(int32_t* out) {
    const auto* facts = world_facts();
    return facts != nullptr && facts->state != nullptr && contracts::succeeded(facts->state(out));
}

bool game_state_gate(int32_t* out) {
    const auto* facts = world_facts();
    return facts != nullptr && facts->game_state_gate != nullptr &&
           contracts::succeeded(facts->game_state_gate(out));
}

::QolResult inventory_item_ref_at(int32_t bag, int32_t slot, InventoryItemRef* out) {
    const auto* api = facts();
    if (api == nullptr || api->item_ref_at == nullptr) {
        return contracts::error(kQolErrNotReady, "not ready", 9);
    }
    return api->item_ref_at(bag, slot, out);
}

::QolResult for_each_bag_slot(BagSlotFn callback, void* ctx) {
    const auto* port = inventory_extras();
    if (port == nullptr || port->read.for_each_bag_slot == nullptr) {
        return contracts::error(kQolErrNotReady, "not ready", 9);
    }
    return port->read.for_each_bag_slot(callback, ctx);
}

void refresh_item_count(InventoryItemRef* ref) {
    if (ref == nullptr || ref->item.slot == 0 || ref->item.generation == 0) return;
    const auto* api = facts();
    contracts::QolItemInventoryView view{};
    if (api == nullptr || api->view == nullptr ||
        !contracts::succeeded(api->view(ref->item, &view)) || view.count_supported == 0) return;
    ref->count = view.effective_count;
}

}  // namespace qol::features::autosell::read_only
