#pragma once

#include <cstdint>

#include "../../contracts/game_host.h"
#include "../../contracts/game_ports/item_port.h"

namespace qol::features::autosell::read_only {

using InventoryItemRef = contracts::QolInventoryItemRef;
using BagSlotFn = contracts::QolInventoryBagSlotFn;

bool item_category(contracts::QolItemHandle item, int32_t* out);
bool item_is_jewel(int32_t category, int32_t* out);
bool item_is_real_equip(contracts::QolItemHandle item, int32_t* out);
bool item_rarity(contracts::QolItemHandle item, int32_t* out);
bool item_enchant_remaining(contracts::QolItemHandle item, int32_t* out);
bool item_socket_filled(contracts::QolItemHandle item, int32_t* out);
bool item_socket_total(contracts::QolItemHandle item, int32_t* out);
bool item_is_backpack(int32_t category, int32_t* out);
bool item_is_dice(int32_t category, int32_t* out);
bool item_is_no_sell(int32_t category, int32_t* out);
bool item_jewel_value(contracts::QolItemHandle item, int32_t* out);
bool item_jewel_attribute_type(contracts::QolItemHandle item, int32_t* out);
bool item_inventory_view(contracts::QolItemHandle item,
                         contracts::QolItemInventoryView* out);

bool world_state(int32_t* out);
bool game_state_gate(int32_t* out);

::QolResult inventory_item_ref_at(int32_t bag, int32_t slot, InventoryItemRef* out);
::QolResult for_each_bag_slot(BagSlotFn callback, void* ctx);

// 物品数量只经 item_count facade 读取；失败时保留库存引用原有数量，保持扫描的
// fail-soft 语义，出售事务仍会在 owner 内重新读取并复核数量。
void refresh_item_count(InventoryItemRef* ref);

}  // namespace qol::features::autosell::read_only
