#pragma once

#include "contracts/inventory_extension.h"

namespace qol::host {

void host_inventory_extension_bind();
bool host_inventory_extension_set_provider(
    const contracts::QolInventoryExtensionProvider* provider, bool (*available_probe)());
const contracts::QolInventoryExtensionProvider* host_inventory_extension_provider_if_resident();
bool host_inventory_extension_resident();
bool host_inventory_extension_available();
QolResult host_inventory_extension_get_access(contracts::QolInventoryExtensionAccess* out_access);

}  // namespace qol::host
