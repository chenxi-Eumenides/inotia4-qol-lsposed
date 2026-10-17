#pragma once

#include "../../contracts/game_host.h"
#include "../../contracts/host_sdk.h"
#include "../../contracts/inventory_slot.h"
#include "../../contracts/item_facts.h"

namespace qol::features::inventory_hooks {

extern const contracts::QolInventorySlotApi kInventorySlotApi;

void inventory_slot_bind(const contracts::QolHostSdk* sdk,
                         const contracts::QolGameHost* game,
                         const contracts::ItemFactsApi* item_facts);

}  // namespace qol::features::inventory_hooks
