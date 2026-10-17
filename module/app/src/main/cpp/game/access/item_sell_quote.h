#pragma once

#include <cstdint>

#include "contracts/item_facts.h"
#include "game/variants/game_variant.h"

namespace qol::game::item_sell_quote {

bool quote(void* item, uint32_t sale_path, GameSeries series,
           contracts::QolSaleQuote* out);

}  // namespace qol::game::item_sell_quote
