#pragma once

#include <cstdint>

#include "contracts/game_host.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/host_sdk.h"
#include "contracts/plugin.h"

namespace qol::features::core_inventory::hooks {

bool attach(const qol::contracts::QolHostSdk* sdk, const qol::contracts::QolGameHost* game,
            const qol::contracts::QolItemPort* port);
void detach();

const qol::contracts::ResourceClaim* claims(uint32_t* out_count);
int32_t bind_backup(const char* resource_id, void* backup);
void set_support(bool enabled);
bool support_enabled();

}  // namespace qol::features::core_inventory::hooks
