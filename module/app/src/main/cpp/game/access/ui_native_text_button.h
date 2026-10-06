#pragma once

#include "game/access/ui_kit.h"

namespace qol::game {

bool ui_draw_native_text_button_background(UiRect rect, uint8_t pressed);
bool ui_draw_native_text_button_label(UiRect rect, const char* text, uint32_t color, int32_t font);

}  // namespace qol::game
