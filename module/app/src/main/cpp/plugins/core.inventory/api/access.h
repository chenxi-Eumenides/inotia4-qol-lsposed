#pragma once

#include "contracts/inventory_access.h"
#include "store/inventory_store.h"

namespace qol::features::core_inventory {

store::InventoryStore& inventory_store();
const contracts::QolInventoryAccessApi& inventory_access_api();

}  // namespace qol::features::core_inventory
