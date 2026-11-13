#pragma once

#include "contracts/plugin.h"
#include <cstdint>

namespace qol::features::core_save {

inline constexpr uint32_t kResourceCount = 4;
const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::core_save
