#pragma once
#include <cstdint>
#include "../../contracts/plugin.h"
#include "../../contracts/game_host.h"
#include "../../contracts/item_payload.h"
#include "../../contracts/inventory_access.h"
#include "../../contracts/inventory_action.h"
#include "../../contracts/game_ports/inventory_extras_port.h"

namespace qol::features::item_lab {
inline constexpr uint32_t kResourceCount = 1;
const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::InventoryActionApi* inventory_action();
const contracts::ItemPayloadApi* item_payload();
const contracts::QolInventoryAccessApi* inventory_access();
const contracts::QolInventoryExtrasPort* inventory_extras();
void item_lab_equip_roll_dice_draw_wrapper();
void item_lab_construct_all();
void item_lab_set_enabled(bool enabled);
bool item_lab_enabled();
}
