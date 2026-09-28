#pragma once

#include "contracts/item_count.h"

namespace qol::game {

using ItemCountActiveProbe = bool (*)();

void item_count_set_provider(const contracts::QolItemCountApi* api);
void item_count_set_active_probe(ItemCountActiveProbe probe);
bool item_count_active();
const contracts::QolItemCountApi& item_count_api();
const contracts::QolItemCountApi* item_count_api_if_resident();

}  // namespace qol::game
