#pragma once

#include "contracts/inventory_projection.h"
#include "contracts/move_merge.h"

namespace qol::features::core_item {

extern const qol::contracts::InventoryProjectionRegistryApi kInventoryProjectionRegistryApi;

// Returns the callbacks registered by the active projection owner, or nullptr.
const qol::contracts::InventoryProjectionCallbacks* projection_callbacks();

}  // namespace qol::features::core_item
