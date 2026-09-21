#include "game/access/ui_choice.h"

#include <cstddef>

#include "game/symbols/game_symbols.h"
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

namespace {

bool writable_global(void* address, size_t size) {
    return address != nullptr && game_memory_accessible(address, size, 'w');
}

}  // namespace

bool ui_choice_write_items(const char* const* items, uint32_t count) {
    if (items == nullptr || count == 0 || count > 6 || g_uichoice_itemtext == nullptr ||
        g_uichoice_count == nullptr ||
        !writable_global(g_uichoice_itemtext, sizeof(char*) * count) ||
        !writable_global(g_uichoice_count, sizeof(uint8_t))) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i] == nullptr) return false;
        static_cast<char**>(g_uichoice_itemtext)[i] = const_cast<char*>(items[i]);
    }
    *static_cast<uint8_t*>(g_uichoice_count) = static_cast<uint8_t>(count);
    return true;
}

bool ui_choice_write_title(const char* title) {
    if (title == nullptr || !writable_global(g_uichoice_main_text, sizeof(char*))) return false;
    *static_cast<char**>(g_uichoice_main_text) = const_cast<char*>(title);
    return true;
}

bool ui_choice_write_focus(int32_t index) {
    if (index < 0 || index > 255 || !writable_global(g_uichoice_focus, sizeof(uint8_t))) {
        return false;
    }
    *static_cast<uint8_t*>(g_uichoice_focus) = static_cast<uint8_t>(index);
    return true;
}

bool ui_choice_push() {
    const int state_id = ui_choice_state_id();
    return state_id >= 0 && fn_popupstate_push != nullptr && fn_popupstate_push(state_id) != 0;
}

bool ui_choice_close_popup() {
    if (fn_ui_set_popup_process_info == nullptr) return false;
    fn_ui_set_popup_process_info(3, 0);
    return true;
}

bool ui_choice_play_sound(int32_t sound_id) {
    if (fn_sound_system_play == nullptr) return false;
    fn_sound_system_play(static_cast<int16_t>(sound_id));
    return true;
}

bool ui_choice_selected_index(int32_t* out_index) {
    if (out_index == nullptr || g_uichoice_control_got == nullptr ||
        fn_control_object_get_cursor_index == nullptr ||
        !game_memory_accessible(g_uichoice_control_got, sizeof(void*), 'r')) {
        return false;
    }
    void* control = *static_cast<void**>(g_uichoice_control_got);
    if (control == nullptr) return false;
    *out_index = fn_control_object_get_cursor_index(control);
    return *out_index >= 0;
}

bool ui_choice_text_owns(const char* const* items, uint32_t count) {
    if (items == nullptr || count == 0 || count > 6 || g_uichoice_itemtext == nullptr ||
        !game_memory_accessible(g_uichoice_itemtext, sizeof(char*) * count, 'r')) {
        return false;
    }
    auto* current = static_cast<char**>(g_uichoice_itemtext);
    for (uint32_t i = 0; i < count; ++i) {
        if (current[i] != items[i]) return false;
    }
    return true;
}

}  // namespace qol::game
