#include "plugins/core.item/api/native_inventory.h"

#include "contracts/host_sdk.h"

namespace qol::features::core_item {
namespace {

const contracts::QolHostSdk* g_native_sdk = nullptr;
const contracts::QolInventoryAccessApi* g_api = nullptr;

}  // namespace

void bind_native_inventory_sdk(const contracts::QolHostSdk* sdk) {
    g_native_sdk = sdk;
    g_api = nullptr;
}

void reset_native_inventory() {
    g_native_sdk = nullptr;
    g_api = nullptr;
}

void invalidate_native_inventory_api() { g_api = nullptr; }

const contracts::QolInventoryAccessApi* inventory_native_api() {
    if (g_api != nullptr) return g_api;
    if (g_native_sdk == nullptr || g_native_sdk->capabilities.require == nullptr) return nullptr;

    const void* iface = nullptr;
    const QolResult result = g_native_sdk->capabilities.require(
        contracts::kInventoryAccessCapabilityId,
        {contracts::kInventoryAccessVersion.major, contracts::kInventoryAccessVersion.minor},
        &iface);
    if (!contracts::succeeded(result) || iface == nullptr) return nullptr;

    const auto* candidate = static_cast<const contracts::QolInventoryAccessApi*>(iface);
    if (!contracts::qol_inventory_native_access_api_valid(candidate)) return nullptr;
    g_api = candidate;
    return g_api;
}

}  // namespace qol::features::core_item
