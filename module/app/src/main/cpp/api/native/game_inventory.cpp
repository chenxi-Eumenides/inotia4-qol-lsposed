#include "game_inventory.h"
#include "game_access.h"
#include "game_state.h"
#include "game_ops_common.h"
#include "core/native/extension_bag_port.h"
#include "core/native/inventory_trade.h"
#include "core/native/qol_log.h"
#include "core/native/stack_codec.h"
#include "core/native/stack_limit_port.h"
#include <cstdint>
#include <string>

#include "game_inventory_basic.inc"
#include "game_inventory_equipment.inc"
#include "game_inventory_use.inc"
#include "game_inventory_read.inc"
