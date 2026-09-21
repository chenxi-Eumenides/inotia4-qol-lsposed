#include "game/access/ui_kit.h"

#include "game/access/game_access.h"
#include "game/symbols/game_symbols.h"

#include <cstring>

namespace {

void set_control_proc(void* ctrl, uintptr_t proc) {
    *reinterpret_cast<uintptr_t*>(static_cast<uint8_t*>(ctrl) + CO_CONTROL_PROC) = proc;
}

void set_execute_proc(void* ctrl, UiClickProc proc) {
    if (proc == nullptr) return;
    uint8_t* data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctrl) + CO_DATA);
    if (data != nullptr) {
        *reinterpret_cast<void**>(data + CB_EXECUTE_PROC) = reinterpret_cast<void*>(proc);
    }
}

void** g_more_games_slot = nullptr;
void* g_more_games_original = nullptr;
void* g_more_games_replacement = nullptr;
uint64_t g_more_games_token = 0;

}  // namespace


#include "game/access/ui_kit_controls.inc"
#include "game/access/ui_kit_render.inc"

void ui_show_text_popup(int32_t text_id) {
    if (g_base == 0) return;
    const uintptr_t popup = g_base + fn_resolve("F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA",
                                                F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA);
    if (popup == 0) return;
    reinterpret_cast<UiPopupMsgCreateOkFromTextDataFn>(popup)(
        static_cast<uint32_t>(text_id), 0, 0, 0);
}

void* ui_main_menu_more_games_button() {
    if (g_base == 0) return nullptr;
    void** slot = reinterpret_cast<void**>(g_base + G_MAINMENU_BASE_VMA + G_MAINMENU_MOREGAMES_SLOT);
    if (!game_memory_accessible(slot, sizeof(void*), 'r')) return nullptr;
    return *slot;
}

bool ui_bind_main_menu_more_games(UiClickProc replacement, uint64_t* out_token) {
    if (out_token != nullptr) *out_token = 0;
    if (replacement == nullptr || out_token == nullptr) return false;
    void* button = ui_main_menu_more_games_button();
    if (button == nullptr) return false;
    auto* data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(button) + CO_DATA);
    if (data == nullptr) return false;
    auto** slot = reinterpret_cast<void**>(data + CB_EXECUTE_PROC);
    if (!game_memory_accessible(slot, sizeof(void*), 'w')) return false;
    if (g_more_games_slot == slot && *slot == reinterpret_cast<void*>(replacement)) {
        *out_token = g_more_games_token;
        return true;
    }
    if (g_more_games_slot != nullptr && game_memory_accessible(g_more_games_slot, sizeof(void*), 'w') &&
        *g_more_games_slot == reinterpret_cast<void*>(replacement)) {
        *g_more_games_slot = g_more_games_original;
    }
    g_more_games_slot = slot;
    g_more_games_original = *slot;
    if (g_more_games_original == nullptr) {
        g_more_games_slot = nullptr;
        return false;
    }
    *slot = reinterpret_cast<void*>(replacement);
    g_more_games_replacement = reinterpret_cast<void*>(replacement);
    g_more_games_token = 1;
    *out_token = g_more_games_token;
    return true;
}

bool ui_unbind_main_menu_more_games(uint64_t token) {
    if (token == 0 || token != g_more_games_token) return false;
    if (g_more_games_slot != nullptr &&
        game_memory_accessible(g_more_games_slot, sizeof(void*), 'w') &&
        *g_more_games_slot == g_more_games_replacement) {
        *g_more_games_slot = g_more_games_original;
    }
    g_more_games_slot = nullptr;
    g_more_games_original = nullptr;
    g_more_games_replacement = nullptr;
    g_more_games_token = 0;
    return true;
}

bool ui_popup_stack_top_matches(void (*enter)()) {
    if (g_base == 0 || enter == nullptr || g_popup_stack == nullptr) return false;
    auto* stack = reinterpret_cast<uint8_t*>(g_popup_stack);
    if (!game_memory_accessible(stack + POPUP_STACK_COUNT, sizeof(uint32_t), 'r')) return false;
    const uint32_t count = *reinterpret_cast<uint32_t*>(stack + POPUP_STACK_COUNT);
    if (count == 0 || count > 27 ||
        !game_memory_accessible(stack + POPUP_STACK_DATA, sizeof(uint64_t), 'r')) return false;
    const uint64_t data = *reinterpret_cast<uint64_t*>(stack + POPUP_STACK_DATA);
    if (data == 0) return false;
    auto* top = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data)) +
                (count - 1) * POPUP_ENTRY_SIZE;
    if (!game_memory_accessible(top + POPUP_ENTRY_ENTER, sizeof(uintptr_t), 'r')) return false;
    return *reinterpret_cast<uintptr_t*>(top + POPUP_ENTRY_ENTER) ==
           reinterpret_cast<uintptr_t>(enter);
}

void ui_clear_control_proc(void* ctrl) {
    if (ctrl == nullptr || !game_memory_accessible(ctrl, CO_CONTROL_PROC + sizeof(uintptr_t), 'w')) {
        return;
    }
    set_control_proc(ctrl, 0);
}

bool ui_delete_control(void* ctrl) {
    if (ctrl == nullptr || fn_touch_handle_delete_control == nullptr) return false;
    fn_touch_handle_delete_control(ctrl);
    return true;
}

bool ui_set_control_item(void* ctrl, void* item) {
    if (ctrl == nullptr || fn_control_item_set_item == nullptr) return false;
    fn_control_item_set_item(ctrl, item);
    return true;
}

bool ui_set_control_proc(void* ctrl, UiControlProc proc) {
    if (ctrl == nullptr || proc == nullptr || fn_control_object_set_control_proc == nullptr) {
        return false;
    }
    fn_control_object_set_control_proc(ctrl, reinterpret_cast<void*>(proc));
    return true;
}

bool ui_set_control_user_type(void* ctrl, int32_t type) {
    if (ctrl == nullptr || fn_control_object_set_user_type == nullptr) return false;
    fn_control_object_set_user_type(ctrl, static_cast<uint32_t>(type));
    return true;
}

bool ui_set_control_data(void* ctrl, void* data) {
    if (ctrl == nullptr || fn_ctrl_set_data == nullptr) return false;
    fn_ctrl_set_data(ctrl, data);
    return true;
}

void* ui_allocate_control_data(size_t size) {
    return fn_mem_malloc == nullptr ? nullptr : fn_mem_malloc(size);
}

bool ui_set_touch_cursor(void* control, void* cursor) {
    if (control == nullptr || fn_touch_handle_set_cursor == nullptr) return false;
    fn_touch_handle_set_cursor(control, cursor);
    return true;
}

bool ui_reset_touch_moving() {
    if (fn_touch_handle_reset_moving_control == nullptr) return false;
    fn_touch_handle_reset_moving_control();
    return true;
}

bool ui_reset_touch_selected() {
    if (fn_touch_handle_reset_selected_control == nullptr) return false;
    (void)fn_touch_handle_reset_selected_control();
    return true;
}
