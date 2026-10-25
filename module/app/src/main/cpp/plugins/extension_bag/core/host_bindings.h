#pragma once

#include "contracts/craft_facts.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/inventory_extras_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"

namespace qol::features::extension_bag {

bool extension_bag_bind_host(const qol::contracts::QolHostSdk* sdk,
                             const qol::contracts::QolGameHost* game,
                             const qol::contracts::QolExtensionBagWritePort* write_port);
void extension_bag_bind_character_extras(
    const qol::contracts::QolCharacterExtrasPort* port);
void extension_bag_bind_inventory_extras(
    const qol::contracts::QolInventoryExtrasPort* port);
void extension_bag_bind_item_facts(const qol::contracts::ItemFactsApi* facts);
const qol::contracts::ItemFactsApi* extension_bag_item_facts();
void extension_bag_bind_craft_facts(const qol::contracts::CraftFactsApi* facts);

}  // namespace qol::features::extension_bag
