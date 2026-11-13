#pragma once

#include "contracts/plugin.h"

namespace qol::features::core_system {

inline constexpr uint32_t kResourceCount = 8;

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::core_system
