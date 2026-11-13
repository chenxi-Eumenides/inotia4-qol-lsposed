#pragma once

#include "contracts/plugin.h"
#include <cstdint>

namespace qol::features::core_feature {

inline constexpr uint32_t kResourceCount = 3;
const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::core_feature
