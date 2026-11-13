#pragma once

#include <cstdint>

#include "../../contracts/plugin.h"

namespace qol::features::damage_widen {

inline constexpr uint32_t kResourceCount = 15;

const contracts::PluginDescriptor& plugin_descriptor();

}
