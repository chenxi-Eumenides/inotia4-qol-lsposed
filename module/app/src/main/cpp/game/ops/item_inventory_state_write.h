#pragma once

#include "contracts/game_ports/item_inventory_state_write_port.h"

namespace qol::game::ops {

QolResult use_item(int32_t bag, int32_t slot,
                   contracts::QolItemInventoryStateWriteResult* out);
QolResult shop_buy(int32_t slot, contracts::QolItemInventoryStateWriteResult* out);
QolResult move_item(int32_t bag, int32_t slot, int32_t count, int32_t to_bag,
                    int32_t to_slot,
                    contracts::QolItemInventoryStateWriteResult* out);
QolResult add_item(int32_t category, int32_t count, int32_t socket_total,
                   int32_t enhance_remaining, int32_t rarity_group,
                   const contracts::QolItemCountApi* item_count,
                   contracts::QolItemInventoryStateWriteResult* out);
QolResult load_item(const uint8_t* payload, uint32_t size, contracts::QolItemHandle* out_item);
QolResult free_loaded_item(contracts::QolItemHandle item);
QolResult place_item(const contracts::QolBagRef* ref, int32_t slot,
                     contracts::QolItemHandle item,
                     contracts::QolItemInventoryStateWriteResult* out);
QolResult remove_item(contracts::QolItemHandle item, int32_t count,
                      contracts::QolItemInventoryStateWriteResult* out);
QolResult equip_item(int32_t role, int32_t bag, int32_t slot,
                     const contracts::QolInventoryExtensionProvider* provider,
                     contracts::QolItemInventoryStateWriteResult* out);
QolResult unequip_item(int32_t role, int32_t equip_slot,
                       const contracts::QolInventoryExtensionProvider* provider,
                       contracts::QolItemInventoryStateWriteResult* out);
QolResult put_jewel(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                    const contracts::QolInventoryExtensionProvider* provider,
                    contracts::QolItemInventoryStateWriteResult* out);
QolResult enchant(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                  const contracts::QolInventoryExtensionProvider* provider,
                  contracts::QolItemInventoryStateWriteResult* out);
// 按 category 删除单件（原版袋经 fn_remove_item 指针路径；扩展袋经中立 provider）。
QolResult remove_item(int32_t category, contracts::QolItemInventoryStateWriteResult* out);

}  // namespace qol::game::ops
