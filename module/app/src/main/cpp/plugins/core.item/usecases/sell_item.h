#pragma once

#include "../../../contracts/plugin.h"
#include "../../../contracts/inventory_action.h"
#include "../../../contracts/inventory_refresh.h"
#include "../../../contracts/item_facts.h"

// Inventory hooks are part of the core.item provider and share its lifecycle.

namespace qol::features::inventory_hooks {

extern const contracts::InventoryRefreshApi kInventoryRefreshApi;
extern const contracts::InventoryActionApi kInventoryActionApi;

bool attach(const contracts::QolHostSdk* sdk, const contracts::QolGameHost* game,
            const contracts::ItemFactsApi* item_facts);
void detach();

const contracts::ResourceClaim* claims(uint32_t* out_count);
int32_t bind_backup(const char* resource_id, void* backup);

}  // namespace qol::features::inventory_hooks
