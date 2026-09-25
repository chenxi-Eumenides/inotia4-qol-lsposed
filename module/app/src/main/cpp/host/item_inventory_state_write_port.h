#pragma once

#include "contracts/game_ports/item_inventory_state_write_port.h"

namespace qol::host {

const contracts::QolItemInventoryStateWritePort& item_inventory_state_write_port();

}  // namespace qol::host
