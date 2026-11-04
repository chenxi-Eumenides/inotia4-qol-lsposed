#pragma once

#include "contracts/inventory_access.h"
#include "contracts/host_sdk.h"

namespace qol::features::core_item {

void bind_native_inventory_sdk(const contracts::QolHostSdk* sdk);
void reset_native_inventory();
const contracts::QolInventoryAccessApi* inventory_native_api();

}  // namespace qol::features::core_item
