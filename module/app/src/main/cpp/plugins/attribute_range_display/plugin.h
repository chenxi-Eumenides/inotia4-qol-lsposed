#pragma once

#include "../../contracts/plugin.h"
#include <cstdint>

namespace qol::features::attribute_range_display {

inline constexpr uint32_t kResourceCount = 2;
const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::attribute_range_display
