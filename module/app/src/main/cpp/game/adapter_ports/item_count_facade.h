#pragma once

#include "contracts/item_count.h"

namespace qol::game {

void item_count_set_provider(const contracts::QolItemCountApi* api);
const contracts::QolItemCountApi& item_count_api();
const contracts::QolItemCountApi* item_count_api_if_resident();

}  // namespace qol::game
