#pragma once

#include "contracts/plugin.h"
#include "contracts/world_facts.h"
#include "contracts/item_facts.h"
#include "contracts/save_facts.h"

namespace qol::features::extension_bag {

const qol::contracts::PluginDescriptor& plugin_descriptor();
const qol::contracts::WorldFactsApi* world_facts();
const qol::contracts::SaveFactsApi* save_facts();

}  // namespace qol::features::extension_bag
