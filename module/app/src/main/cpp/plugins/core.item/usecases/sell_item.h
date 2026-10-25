#pragma once

#include "../../../contracts/plugin.h"
#include "../../../contracts/inventory_action.h"
#include "../../../contracts/inventory_refresh.h"
#include "../../../contracts/item_facts.h"
#include "../../../contracts/game_ports/item_port.h"

// Inventory hooks are part of the core.item provider and share its lifecycle.

namespace qol::features::inventory_hooks {

namespace detail {
inline const contracts::QolItemPort*& bound_item_port() {
    static const contracts::QolItemPort* item_port = nullptr;
    return item_port;
}
}  // namespace detail

extern const contracts::InventoryRefreshApi kInventoryRefreshApi;
extern const contracts::InventoryActionApi kInventoryActionApi;

bool attach(const contracts::QolHostSdk* sdk, const contracts::QolGameHost* game,
            const contracts::ItemFactsApi* item_facts);
inline bool attach(const contracts::QolHostSdk* sdk, const contracts::QolGameHost* game,
                   const contracts::ItemFactsApi* item_facts,
                   const contracts::QolItemPort* item_port) {
    detail::bound_item_port() = item_port;
    return attach(sdk, game, item_facts);
}
void detach();

const contracts::ResourceClaim* claims(uint32_t* out_count);
int32_t bind_backup(const char* resource_id, void* backup);

}  // namespace qol::features::inventory_hooks
