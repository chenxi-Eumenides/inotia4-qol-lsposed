#pragma once

#include <cstdint>

namespace qol::game {

using UiPopupCallback = void (*)(void* param);

bool ui_confirmation_popup_active(uint8_t* out_active);
bool ui_confirmation_popup_get_callbacks(UiPopupCallback* out_ok,
                                          UiPopupCallback* out_cancel);
bool ui_confirmation_popup_replace_callback_if(uint8_t is_ok, UiPopupCallback expected,
                                                UiPopupCallback replacement,
                                                uint8_t* out_replaced);
bool ui_confirmation_popup_create(uint32_t text_id, uint32_t display_type,
                                  uint32_t format_arg, UiPopupCallback ok,
                                  UiPopupCallback cancel, uint32_t numeric_param);
// Raw-text YesNo (UIPopupMsg_CreateYesNo, type=2). text is referenced by the game until bOn
// returns to 0; the caller owns its lifetime. Same resolved symbol as world_create_yesno.
bool ui_confirmation_popup_create_text(const char* text, uint32_t length,
                                       uint32_t display_type, uint32_t numeric_param,
                                       UiPopupCallback ok, UiPopupCallback cancel);
bool ui_confirmation_popup_close_if_callbacks(UiPopupCallback expected_ok,
                                               UiPopupCallback expected_cancel,
                                               uint8_t* out_closed);
// Draws the active native UIPopupMsg with the game's own renderer. Callers must verify
// ui_confirmation_popup_active() first; this wrapper only validates thread/function readiness.
bool ui_confirmation_popup_draw();

}  // namespace qol::game
