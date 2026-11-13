#pragma once

#include "../../contracts/plugin.h"

namespace qol::features::combat_move {

const contracts::PluginDescriptor& plugin_descriptor();
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);
void keep_target_wrapper(void* character, void* target);
int32_t move_gate_wrapper(void* character);
void set_action_wrapper(void* character, void* action, void* target);
int32_t start_action_ex_wrapper(void* character, void* action, int32_t flag);

}  // namespace qol::features::combat_move
