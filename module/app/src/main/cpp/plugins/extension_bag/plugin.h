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

const qol::contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::extension_bag
