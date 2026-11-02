#pragma once

#include "contracts/inventory_action.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/state_access.h"

namespace qol::features::extension_bag {

// Inventory actions are looked up at each user operation so revoked capability
// leases are never retained across calls. UI callbacks run on the game thread.
inline const contracts::InventoryActionApi* inventory_action_api() {
    static bool missing_logged = false;
    const auto* sdk = ::qol::extension_bag::access::host().sdk;
    const void* iface = nullptr;
    constexpr char kUnavailable[] = "inventory.action unavailable";
    QolResult result = contracts::error(
        kQolErrNotReady, kUnavailable, static_cast<uint32_t>(sizeof(kUnavailable) - 1));
    if (sdk != nullptr && sdk->capabilities.require != nullptr) {
        result = sdk->capabilities.require(
            contracts::kInventoryActionCapabilityId,
            {contracts::kInventoryActionVersionMajor,
             contracts::kInventoryActionVersionMinor},
            &iface);
    }
    const auto* api = contracts::succeeded(result)
                          ? static_cast<const contracts::InventoryActionApi*>(iface)
                          : nullptr;
    if (contracts::inventory_action_api_valid(api)) return api;
    if (!missing_logged) {
        missing_logged = true;
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "inventory.action unavailable code=%d; item mutation rejected",
                      result.code);
    }
    return nullptr;
}

}  // namespace qol::features::extension_bag
