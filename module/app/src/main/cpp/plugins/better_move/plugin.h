#pragma once

#include "../../contracts/plugin.h"
#include <cstdint>

namespace qol::features::better_move {

inline constexpr uint32_t kResourceCount = 8;
const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::better_move
