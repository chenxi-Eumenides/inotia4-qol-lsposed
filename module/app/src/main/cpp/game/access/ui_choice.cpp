#include "game/access/ui_choice.h"

#include "data/native/game_symbols.h"
#include "game/access/popup_state.h"
#include "game_access.h"

namespace qol::game {

int ui_choice_state_id() {
    PopupStateSlot slot{};
    if (!popup_state_locate(F_PANEL_CHOICE_ENTER, &slot)) return -1;
    return slot.state_id;
}

bool ui_choice_button_slot_target(void** out_slot, uintptr_t* out_expected) {
    if (out_slot == nullptr || out_expected == nullptr) return false;
    if (g_uichoice_button_list_exe_got == nullptr || fn_uichoice_button_list_exe == nullptr) {
        return false;
    }
    *out_slot = g_uichoice_button_list_exe_got;
    *out_expected = reinterpret_cast<uintptr_t>(fn_uichoice_button_list_exe);
    return true;
}

}  // namespace qol::game
