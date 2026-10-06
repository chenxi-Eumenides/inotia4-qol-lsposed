#pragma once

#include "game/access/ui_kit.h"

namespace qol::game {

// Draws one native Options capsule within rect. Returns false without drawing on invalid input
// or when the game-owned image resources/primitives are unavailable.
bool ui_draw_native_toggle(UiRect rect, int32_t value, uint8_t pressed);

}  // namespace qol::game
