#pragma once

#include <cstdint>

#include "../../contracts/plugin.h"
#include "../../contracts/game_ports/item_extras_port.h"

namespace qol::features::attribute_range_display {

inline constexpr const char* kResourceIdAddOption = "hook:UIDesc_AddOption";
inline constexpr const char* kResourceIdMakeItem = "hook:UIDesc_MakeItem";

void set_context(const contracts::PluginContext& ctx);
void set_active(bool active);
bool active();

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

void uid_desc_add_option_wrapper(void* builder, int type, int option_index, int value);
uintptr_t uid_desc_make_item_wrapper(void* item, void* character, void* arg2);

}  // namespace qol::features::attribute_range_display
