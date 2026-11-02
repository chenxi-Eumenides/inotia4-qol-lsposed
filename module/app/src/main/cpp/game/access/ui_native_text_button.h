#pragma once

#include "contracts/ui.h"
#include "game/access/ui_kit.h"

namespace qol::game {

bool ui_draw_native_text_button_background(UiRect rect, uint8_t pressed);
bool ui_draw_native_text_button_label(UiRect rect, const char* text, uint32_t color, int32_t font);

// Explicit appearance helpers shared by both factories and the menu binding.
// Layout preflights the game-owned resources and supported font size without drawing.
// Draw validates both background and label before painting and preserves the active clip.
// No helper allocates private fonts or changes the legacy entry points' geometry.
::QolResult ui_layout_native_text_button(const contracts::UiTextButtonPaintDesc& desc,
                                        contracts::UiTextButtonPaintLayout* out);
::QolResult ui_draw_native_text_button(const contracts::UiTextButtonPaintDesc& desc,
                                      const char* text, uint32_t text_color, uint8_t pressed);

}  // namespace qol::game
