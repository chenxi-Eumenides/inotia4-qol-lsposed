#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include "contracts/plugin.h"
#include "contracts/item_facts.h"

namespace qol::features::extension_bag {

inline constexpr uint32_t kResourceCount = 31;

const qol::contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::extension_bag
