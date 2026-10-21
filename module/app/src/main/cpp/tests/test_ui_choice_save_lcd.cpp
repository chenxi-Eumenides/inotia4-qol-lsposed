// game/access/ui_choice.cpp: push no longer snapshots the framebuffer. The
// Scene_Draw_POPUP_SC_CHOICE wrapper owns SaveLCD (before dimming), so a push-time snapshot
// would be overwritten by the refresh-flag branch. Failed pushes must not save either.
#include <cassert>
#include <cstdint>

#include "game/access/game_access.h"
#include "game/access/popup_state.h"
#include "game/access/ui_choice.h"
#include "game/symbols/game_symbols.h"

namespace {
int g_push_result = 0;
int g_push_calls = 0;
int g_save_calls = 0;

int fake_popupstate_push(uint32_t) {
    ++g_push_calls;
    return g_push_result;
}
}  // namespace

bool game_memory_accessible(const void*, size_t, char) { return true; }

void* g_uichoice_itemtext = nullptr;
void* g_uichoice_count = nullptr;
void* g_uichoice_focus = nullptr;
void* g_uichoice_main_text = nullptr;
void* g_uichoice_button_list_exe_got = nullptr;
void* g_uichoice_control_got = nullptr;
UiChoiceButtonListExeFn fn_uichoice_button_list_exe = nullptr;
UiChoiceDrawFn fn_uichoice_draw = nullptr;

UiSetPopupProcessInfoFn fn_ui_set_popup_process_info = nullptr;
PopupStatePushFn fn_popupstate_push = &fake_popupstate_push;
ControlObjectGetCursorIndexFn fn_control_object_get_cursor_index = nullptr;
SoundSystemPlayFn fn_sound_system_play = nullptr;
UiGetRefreshLcdFlagFn fn_ui_get_refresh_lcd_flag = nullptr;
UiSetRefreshLcdFlagFn fn_ui_set_refresh_lcd_flag = nullptr;
GrpxStartFn fn_grpx_start = nullptr;
GrpxEndFn fn_grpx_end = nullptr;

namespace qol::game {
bool popup_state_locate(uintptr_t, PopupStateSlot* out) {
    if (out == nullptr) return false;
    out->state_id = 42;
    return true;
}
int64_t frame_count() { return 0; }
bool game_host_is_main_thread() { return true; }
}  // namespace qol::game

bool ui_save_lcd_now() {
    ++g_save_calls;
    return true;
}

int main() {
    g_push_result = 0;
    g_push_calls = g_save_calls = 0;
    assert(!qol::game::ui_choice_push());
    assert(g_push_calls == 1 && g_save_calls == 0);

    g_push_result = 1;
    assert(qol::game::ui_choice_push());
    assert(g_push_calls == 2 && g_save_calls == 0);
    return 0;
}
