#pragma once

#include <cstdint>

#include "../../../contracts/craft_facts.h"
#include "../../../contracts/game_ports/craft_ui_write_port.h"
#include "../../../contracts/game_ports/custom_recipe_write_port.h"
#include "../../../contracts/item_facts.h"
#include "../../../contracts/plugin.h"
#include "../../../contracts/text_layer.h"

namespace qol::features::custom_recipe::ui {

const contracts::ResourceClaim* claims(uint32_t* out_count);
bool core_backups_ready();
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id,
                          void* backup);

bool bind(contracts::PluginContext& ctx, const contracts::ItemFactsApi* item_facts,
          const contracts::CraftFactsApi* craft_facts,
          const contracts::QolCustomRecipeWritePort* recipe_write_port,
          const contracts::QolCraftUiWritePort* ui_write_port,
          const contracts::TextLayerApi* text_api);
void clear();

}  // namespace qol::features::custom_recipe::ui
