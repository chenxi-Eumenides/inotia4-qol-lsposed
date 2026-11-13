#pragma once

#include "../../contracts/plugin.h"
#include <cstdint>

namespace qol::features::combat_move {

inline constexpr uint32_t kResourceCount = 4;
const contracts::PluginDescriptor& plugin_descriptor();
// 攻击范围余量后的有效距离：距离越大余量越小（4→+100%，13→+10%）。
int32_t effective_range(int32_t range);
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);
void keep_target_wrapper(void* character, void* target);
int32_t move_gate_wrapper(void* character);
void set_action_wrapper(void* character, void* action, void* target);
int32_t start_action_ex_wrapper(void* character, void* action, int32_t flag);

}  // namespace qol::features::combat_move
