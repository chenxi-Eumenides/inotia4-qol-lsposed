#pragma once

#include <cstdint>

#include "../../contracts/plugin.h"

namespace qol::features::custom_map {

// 面板 provider 声明数（PluginDescriptor.panel_provider_count）。
inline constexpr uint32_t kProviderCount = 1;
const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::custom_map
