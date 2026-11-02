#pragma once

#include "game/access/ui_kit.h"

namespace qol::game {

// Draws one native X close glyph within rect (unit 0x0f loc 0x3a); pressed overlays the
// orange pressed plate (loc 0x34) centered on the normal glyph. Hit testing, press/cancel
// and click semantics belong to the owning control, not to this primitive. Returns false
// without drawing on invalid input or when the game-owned resources/primitives are
// unavailable.
bool ui_draw_native_close_button(UiRect rect, uint8_t pressed);

}  // namespace qol::game
