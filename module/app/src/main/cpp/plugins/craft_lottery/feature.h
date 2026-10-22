#pragma once

#include <cstdint>

#include "../../contracts/game_ports/craft_lottery_port.h"
#include "../../contracts/plugin.h"

namespace qol::features::craft_lottery {

const contracts::QolHostSdk* host_sdk();
const contracts::QolCraftLotteryWritePort* craft_lottery_write_port();

inline constexpr const char* kResourceIdProcessRecipe = "hook:CHAR_ProcessRecipe";

void set_gate(bool enabled);
bool gate_enabled();

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);
int process_recipe_wrapper(uint32_t use_type);

}  // namespace qol::features::craft_lottery
