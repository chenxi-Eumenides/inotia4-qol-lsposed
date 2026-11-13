#pragma once

#include "../../contracts/plugin.h"
#include <cstdint>

namespace qol::features::craft_lottery {

inline constexpr uint32_t kResourceCount = 1;
const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::craft_lottery
