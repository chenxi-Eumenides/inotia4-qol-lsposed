#pragma once

#include "contracts/game_ports/item_inventory_state_write_port.h"

namespace qol::game::ops {

QolResult use_item(int32_t bag, int32_t slot,
                   contracts::QolItemInventoryStateWriteResult* out);
QolResult shop_buy(int32_t slot, contracts::QolItemInventoryStateWriteResult* out);
QolResult move_item(int32_t bag, int32_t slot, int32_t count, int32_t to_bag,
                    int32_t to_slot,
                    contracts::QolItemInventoryStateWriteResult* out);

}  // namespace qol::game::ops
