#pragma once

#include "contracts/plugin.h"
#include <cstdint>

namespace qol::features::core_inventory {

inline constexpr uint32_t kResourceCount = 12;
const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::core_inventory
