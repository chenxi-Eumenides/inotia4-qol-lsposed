#include "game/adapter_ports/inventory_extension_facade.h"

namespace qol::game {
namespace {

const contracts::QolInventoryExtensionProvider* g_provider = nullptr;
InventoryExtensionAvailabilityProbe g_available_probe = nullptr;
bool g_host_bound = false;

}  // namespace

void inventory_extension_bind_host() { g_host_bound = true; }

bool inventory_extension_set_provider(
    const contracts::QolInventoryExtensionProvider* provider,
    InventoryExtensionAvailabilityProbe available_probe) {
    if (!g_host_bound || provider == nullptr || available_probe == nullptr) return false;
    if (g_provider == nullptr && g_available_probe == nullptr) {
        g_provider = provider;
        g_available_probe = available_probe;
        return true;
    }
    return g_provider == provider && g_available_probe == available_probe;
}

const contracts::QolInventoryExtensionProvider* inventory_extension_provider_if_resident() {
    return g_provider;
}

bool inventory_extension_resident() { return g_provider != nullptr; }

bool inventory_extension_available() {
    return g_provider != nullptr && g_available_probe != nullptr && g_available_probe();
}

QolResult inventory_extension_get_access(contracts::QolInventoryExtensionAccess* out_access) {
    if (out_access == nullptr) {
        static constexpr char kMessage[] = "invalid argument";
        return contracts::error(kQolErrInvalidArgument, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    *out_access = {};
    if (g_provider == nullptr) {
        static constexpr char kMessage[] = "not ready";
        return contracts::error(kQolErrNotReady, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    out_access->provider = g_provider;
    out_access->resident = 1;
    out_access->available = inventory_extension_available() ? 1 : 0;
    return contracts::ok();
}

}  // namespace qol::game
