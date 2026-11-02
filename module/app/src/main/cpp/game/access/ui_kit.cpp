#include "game/access/ui_kit.h"

#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_handle_registry.h"
#include "game/adapter_ports/game_host_port.h"
#include "game/clock.h"
#include "game/state/game_state.h"
#include "game/symbols/game_symbols.h"

#include <array>
#include <cstring>

namespace {

int64_t g_main_menu_first_seen_frame = -1;

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

void ui_main_menu_observe_screen(const char* screen) {
    if (!qol::game::game_host_is_main_thread()) return;
    if (screen == nullptr || std::strcmp(screen, "main_menu") != 0) {
        g_main_menu_first_seen_frame = -1;
        return;
    }
    if (g_main_menu_first_seen_frame < 0) {
        g_main_menu_first_seen_frame = qol::game::frame_count();
    }
}

bool ui_main_menu_live_tree_snapshot(void* probe, void** out_original, void** out_parent,
                                     int32_t* out_original_in_tree,
                                     int32_t* out_probe_in_tree) {
    if (out_original == nullptr || out_parent == nullptr || out_original_in_tree == nullptr ||
        out_probe_in_tree == nullptr) return false;
    *out_original = nullptr;
    *out_parent = nullptr;
    *out_original_in_tree = 0;
    *out_probe_in_tree = 0;
    if (!qol::game::game_host_is_main_thread() || g_base == 0 ||
        fn_ctrl_get_count == nullptr || fn_ctrl_get_child == nullptr) return false;

    const char* screen = ui_screen();
    ui_main_menu_observe_screen(screen);
    if (screen == nullptr || std::strcmp(screen, "main_menu") != 0) return false;
    // GAMESTATE_SetState publishes state 4 before its synchronous STATE_EnterMainMenu callback
    // creates the tree; wait for the next observed frame before touching the retained root slot.
    const int64_t frame = qol::game::frame_count();
    if (frame < 0 || g_main_menu_first_seen_frame < 0 ||
        frame <= g_main_menu_first_seen_frame) return false;

    // STATE_EnterMainMenu creates this group at +0x08 and passes it to ControlButton_Create
    // for the More Games slot at +0x20. STATE_ExitMainMenu deletes it but leaves the static slot
    // (.tmp/main-menu-adapter-followup/evidence.md §1).
    auto** root_slot = reinterpret_cast<void**>(g_base + G_MAINMENU_BASE_VMA + G_MAINMENU_ROOT_SLOT);
    if (!game_memory_accessible(root_slot, sizeof(*root_slot), 'r')) return false;
    void* root = *root_slot;
    if (root == nullptr || !game_memory_accessible(root, CO_SIZE, 'r')) return false;

    // Enumerate the engine-owned live root before examining the original slot's object.
    constexpr uint32_t kMaxMainMenuChildren = 128;
    const uint32_t count = fn_ctrl_get_count(root);
    if (count == 0 || count > kMaxMainMenuChildren) return false;
    std::array<void*, kMaxMainMenuChildren> children{};
    for (uint32_t index = 0; index < count; ++index) {
        void* child = fn_ctrl_get_child(root, index);
        if (child == nullptr) return false;
        children[index] = child;
    }

    auto** original_slot = reinterpret_cast<void**>(
        g_base + G_MAINMENU_BASE_VMA + G_MAINMENU_MOREGAMES_SLOT);
    if (!game_memory_accessible(original_slot, sizeof(*original_slot), 'r')) return false;
    void* original = *original_slot;
    *out_original = original;
    *out_parent = root;
    for (uint32_t index = 0; index < count; ++index) {
        if (children[index] == original) *out_original_in_tree = 1;
        if (probe != nullptr && children[index] == probe) *out_probe_in_tree = 1;
    }
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
