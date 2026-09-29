#include "host/inventory_extension_facade.h"

#include "game/adapter_ports/inventory_extension_facade.h"

namespace qol::host {

void host_inventory_extension_bind() { qol::game::inventory_extension_bind_host(); }

bool host_inventory_extension_set_provider(
    const contracts::QolInventoryExtensionProvider* provider, bool (*available_probe)()) {
    return qol::game::inventory_extension_set_provider(provider, available_probe);
}

const contracts::QolInventoryExtensionProvider* host_inventory_extension_provider_if_resident() {
    return qol::game::inventory_extension_provider_if_resident();
}

bool host_inventory_extension_resident() { return qol::game::inventory_extension_resident(); }

bool host_inventory_extension_available() { return qol::game::inventory_extension_available(); }

QolResult host_inventory_extension_get_access(contracts::QolInventoryExtensionAccess* out_access) {
    return qol::game::inventory_extension_get_access(out_access);
}

}  // namespace qol::host
