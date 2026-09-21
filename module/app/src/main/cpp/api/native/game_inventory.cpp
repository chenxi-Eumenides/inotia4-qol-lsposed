#include "game_inventory.h"
#include "game_access.h"
#include "game_state.h"
#include "game/ops/game_ops_common.h"
#include "contracts/extension_bag_port.h"
#include "core/native/inventory_trade.h"
#include "infra/log/qol_log.h"
#include <cstdint>
#include <string>

#include "game_inventory_basic.inc"
#include "game_inventory_equipment.inc"
#include "game_inventory_use.inc"
#include "game_inventory_read.inc"
