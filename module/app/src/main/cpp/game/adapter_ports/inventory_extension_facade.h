#pragma once

#include "contracts/inventory_extension.h"

namespace qol::game {

using InventoryExtensionAvailabilityProbe = bool (*)();

// Provider callbacks run on the game main thread. They must not re-enter the provider while
// g_virtual_bag_mtx is held.
bool inventory_extension_set_provider(
    const contracts::QolInventoryExtensionProvider* provider,
    InventoryExtensionAvailabilityProbe available_probe);
const contracts::QolInventoryExtensionProvider* inventory_extension_provider_if_resident();
bool inventory_extension_resident();
bool inventory_extension_available();
QolResult inventory_extension_get_access(contracts::QolInventoryExtensionAccess* out_access);
void inventory_extension_bind_host();

}  // namespace qol::game
