#pragma once

#include <cstdint>

#include "game/access/ui_kit.h"

namespace qol::game {

// Draws the shared unit 0x0f container frame and its single optional interior fill.
// The caller must be on the game main thread.
bool ui_draw_container_frame(UiRect rect, uint32_t fill_rgb565, uint32_t alpha);

}  // namespace qol::game
