#pragma once

#include <cstdint>

#include "../../contracts/ui.h"

namespace qol::plugins::custom_ui {

enum class UiConfirmationOverlayHit { kNone, kConfirm, kCancel };

struct UiConfirmationOverlayContent {
    const char* message;
    const char* confirm_label;
    const char* cancel_label;
    bool has_price;
    uint32_t price;
    const char* price_unit;
};

struct UiConfirmationOverlayLayout {
    contracts::UiRect mask_top;
    contracts::UiRect mask_bottom;
    contracts::UiRect mask_left;
    contracts::UiRect mask_right;
    contracts::UiRect panel;
    contracts::UiRect text;
    contracts::UiRect price;
    contracts::UiRect confirm_button;
    contracts::UiRect cancel_button;
    uint32_t screen_w;
    uint32_t screen_h;
    uint8_t has_price;
    uint8_t valid;
};

bool ui_confirmation_overlay_layout(uint32_t screen_w, uint32_t screen_h,
                                    const UiConfirmationOverlayContent& content,
                                    UiConfirmationOverlayLayout& out);
bool ui_confirmation_overlay_draw(contracts::QolUiAdapter& ui,
                                  const UiConfirmationOverlayContent& content,
                                  const UiConfirmationOverlayLayout& layout);
UiConfirmationOverlayHit ui_confirmation_overlay_hit_test(
    const UiConfirmationOverlayLayout& layout, int32_t x, int32_t y);

}  // namespace qol::plugins::custom_ui
