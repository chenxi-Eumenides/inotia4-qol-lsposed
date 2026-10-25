#pragma once

#include "contracts/game_host.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/move_merge_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/move_merge.h"
#include "contracts/plugin.h"

namespace qol::features::core_item::merge {

extern const contracts::MoveMergeControlApi kMoveMergeControlApi;

const contracts::ResourceClaim* claims(uint32_t* out_count);
bool attach(const contracts::QolHostSdk* sdk,
            const contracts::QolMoveMergePort* move_port,
            const contracts::QolCharacterExtrasPort* character_port,
            const contracts::ItemFactsApi* item_facts);
void detach();
void set_enabled(bool enabled);
bool enabled();
bool wrapper_ready();
void set_wrapper_ready(bool ready);
bool is_merge_resource(const char* resource_id);

}  // namespace qol::features::core_item::merge
