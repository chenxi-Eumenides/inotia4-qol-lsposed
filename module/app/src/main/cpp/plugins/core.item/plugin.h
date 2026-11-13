#pragma once

#include "contracts/plugin.h"
#include "contracts/item_facts.h"
#include <cstdint>

namespace qol::features::core_item {

inline constexpr uint32_t kResourceCount = 10;
const contracts::PluginDescriptor& plugin_descriptor();
const contracts::ItemFactsApi* item_facts_api();

}  // namespace qol::features::core_item
