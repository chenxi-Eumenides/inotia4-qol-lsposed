#include "plugins/core.item/api/projection.h"

#include <atomic>

namespace qol::features::core_item {
namespace {

std::atomic<const qol::contracts::InventoryProjectionCallbacks*> g_projection_callbacks{nullptr};

QolResult register_callbacks(const qol::contracts::InventoryProjectionCallbacks* callbacks) {
    if (!qol::contracts::inventory_projection_callbacks_valid(callbacks)) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid projection callbacks", 64);
    }
    g_projection_callbacks.store(callbacks, std::memory_order_release);
    return qol::contracts::ok();
}

QolResult unregister_callbacks() {
    g_projection_callbacks.store(nullptr, std::memory_order_release);
    return qol::contracts::ok();
}

}  // namespace

const qol::contracts::InventoryProjectionRegistryApi kInventoryProjectionRegistryApi{
    sizeof(qol::contracts::InventoryProjectionRegistryApi),
    &register_callbacks,
    &unregister_callbacks,
};

const qol::contracts::InventoryProjectionCallbacks* projection_callbacks() {
    return g_projection_callbacks.load(std::memory_order_acquire);
}

}  // namespace qol::features::core_item
