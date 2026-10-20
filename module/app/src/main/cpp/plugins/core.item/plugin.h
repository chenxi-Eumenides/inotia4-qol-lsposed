#pragma once

#include "contracts/plugin.h"
#include "contracts/item_facts.h"

namespace qol::features::core_item {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::ItemFactsApi* item_facts_api();

}  // namespace qol::features::core_item
