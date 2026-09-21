#include "read_only.h"

#include "plugin.h"

#include <limits>

namespace qol::features::autosell::read_only {
namespace {

const contracts::QolGameHost* game() { return game_host(); }

}  // namespace

bool item_category(const void* item, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.category != nullptr &&
           contracts::succeeded(host->read_only.item.category(item, out));
}

bool item_is_jewel(int32_t category, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.is_jewel_category != nullptr &&
           contracts::succeeded(host->read_only.item.is_jewel_category(category, out));
}

bool item_is_real_equip(const void* item, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.is_real_equip != nullptr &&
           contracts::succeeded(host->read_only.item.is_real_equip(item, out));
}

bool item_rarity(const void* item, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.rarity != nullptr &&
           contracts::succeeded(host->read_only.item.rarity(item, out));
}

bool item_enchant_remaining(const void* item, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.enchant_remaining != nullptr &&
           contracts::succeeded(host->read_only.item.enchant_remaining(item, out));
}

bool item_socket_filled(const void* item, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.socket_filled != nullptr &&
           contracts::succeeded(host->read_only.item.socket_filled(item, out));
}

bool item_socket_total(const void* item, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.socket_total != nullptr &&
           contracts::succeeded(host->read_only.item.socket_total(item, out));
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
    const auto* host = game();
    return host != nullptr && host->read_only.item.is_no_sell != nullptr &&
           contracts::succeeded(host->read_only.item.is_no_sell(category, out));
}

bool item_jewel_value(const void* item, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.jewel_value != nullptr &&
           contracts::succeeded(host->read_only.item.jewel_value(item, out));
}

bool item_jewel_attribute_type(const void* item, int32_t* out) {
    const auto* host = game();
    return host != nullptr && host->read_only.item.jewel_attribute_type != nullptr &&
           contracts::succeeded(host->read_only.item.jewel_attribute_type(item, out));
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
    const auto* host = game();
    if (host == nullptr || host->read_only.inventory.item_ref_at == nullptr) {
        return contracts::error(kQolErrNotReady, "not ready", 9);
    }
    return host->read_only.inventory.item_ref_at(bag, slot, out);
}

::QolResult for_each_bag_slot(BagSlotFn callback, void* ctx) {
    const auto* host = game();
    if (host == nullptr || host->read_only.inventory.for_each_bag_slot == nullptr) {
        return contracts::error(kQolErrNotReady, "not ready", 9);
    }
    return host->read_only.inventory.for_each_bag_slot(callback, ctx);
}

void refresh_item_count(InventoryItemRef* ref) {
    if (ref == nullptr || ref->native_item == nullptr) return;
    const auto* host = game();
    if (host == nullptr || host->item_count.effective == nullptr) return;
    uint32_t count = 0;
    if (!contracts::succeeded(host->item_count.effective(ref->native_item, &count)) ||
        count > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return;
    }
    ref->count = static_cast<int32_t>(count);
}

}  // namespace qol::features::autosell::read_only
