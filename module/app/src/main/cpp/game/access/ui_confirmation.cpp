#include "game/access/ui_confirmation.h"

#include "game/access/game_access.h"
#include "game/adapter_ports/game_host_port.h"
#include "game/access/ui_kit.h"

#include <cstdint>

namespace qol::game {
namespace {

using CallbackSlot = UiPopupCallback*;

bool main_thread_ready() { return game_host_is_main_thread(); }

bool popup_active_slot(uint8_t** out) {
    if (out == nullptr || g_base == 0) return false;
    auto* slot = reinterpret_cast<uint8_t*>(g_base + G_POPUP_ON_VMA);
    if (!game_memory_accessible(slot, sizeof(*slot), 'r')) return false;
    *out = slot;
    return true;
}

bool callback_slots(CallbackSlot* out_ok, CallbackSlot* out_cancel, char permission) {
    if (out_ok == nullptr || out_cancel == nullptr || g_base == 0) return false;
    auto* ok = reinterpret_cast<CallbackSlot>(g_base + G_POPUP_FPOK_VMA);
    auto* cancel = reinterpret_cast<CallbackSlot>(g_base + G_POPUP_FPCANCEL_VMA);
    if (!game_memory_accessible(ok, sizeof(*ok), permission) ||
        !game_memory_accessible(cancel, sizeof(*cancel), permission)) {
        return false;
    }
    *out_ok = ok;
    *out_cancel = cancel;
    return true;
}

bool native_function_ready(const void* function) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(function);
    return function != nullptr && address != g_base &&
        game_memory_accessible(function, sizeof(uint32_t), 'x');
}

}  // namespace

bool ui_confirmation_popup_active(uint8_t* out_active) {
    if (out_active == nullptr || !main_thread_ready()) return false;
    *out_active = 0;
    uint8_t* slot = nullptr;
    if (!popup_active_slot(&slot)) return false;
    *out_active = *slot != 0 ? 1 : 0;
    return true;
}

bool ui_confirmation_popup_get_callbacks(UiPopupCallback* out_ok,
                                         UiPopupCallback* out_cancel) {
    if (out_ok == nullptr || out_cancel == nullptr || !main_thread_ready()) return false;
    *out_ok = nullptr;
    *out_cancel = nullptr;
    CallbackSlot ok = nullptr;
    CallbackSlot cancel = nullptr;
    if (!callback_slots(&ok, &cancel, 'r')) return false;
    *out_ok = *ok;
    *out_cancel = *cancel;
    return true;
}

bool ui_confirmation_popup_replace_callback_if(uint8_t is_ok, UiPopupCallback expected,
                                                 UiPopupCallback replacement,
                                                 uint8_t* out_replaced) {
    if (is_ok > 1 || out_replaced == nullptr || !main_thread_ready()) return false;
    *out_replaced = 0;
    CallbackSlot ok = nullptr;
    CallbackSlot cancel = nullptr;
    if (!callback_slots(&ok, &cancel, 'w')) return false;
    CallbackSlot slot = is_ok != 0 ? ok : cancel;
    if (*slot != expected) return true;
    *slot = replacement;
    *out_replaced = 1;
    return true;
}

bool ui_confirmation_popup_create(uint32_t text_id, uint32_t display_type,
                                  uint32_t format_arg, UiPopupCallback ok,
                                  UiPopupCallback cancel, uint32_t numeric_param) {
    if (!main_thread_ready() || ok == nullptr || cancel == nullptr || g_base == 0 ||
        !native_function_ready(reinterpret_cast<const void*>(fn_popup_create_yesno_from_textdata))) {
        return false;
    }
    uint8_t active = 0;
    if (!ui_confirmation_popup_active(&active) || active != 0) return false;
    static_assert(sizeof(uintptr_t) >= sizeof(numeric_param),
                  "native callback param must carry the numeric price");
    fn_popup_create_yesno_from_textdata(
        text_id, display_type, format_arg, reinterpret_cast<void*>(ok),
        reinterpret_cast<void*>(cancel), reinterpret_cast<void*>(static_cast<uintptr_t>(numeric_param)));
    return true;
}

bool ui_confirmation_popup_create_text(const char* text, uint32_t length,
                                       uint32_t display_type, uint32_t numeric_param,
                                       UiPopupCallback ok, UiPopupCallback cancel) {
    if (!main_thread_ready() || text == nullptr || ok == nullptr || cancel == nullptr ||
        g_base == 0 ||
        !native_function_ready(reinterpret_cast<const void*>(fn_popup_create_yesno))) {
        return false;
    }
    uint8_t active = 0;
    if (!ui_confirmation_popup_active(&active) || active != 0) return false;
    static_assert(sizeof(uintptr_t) >= sizeof(numeric_param),
                  "native callback param must carry the numeric price");
    // Native signature: (text, len, x2, x3, ok, cancel, param). x2 is forwarded to
    // CreateButtonControl (0 for module dialogs); x3 is i32DisplayType (0=plain body,
    // 2=price layout) and is the only layout selector. The internal i32Type is fixed.
    // Layout is derived from price so callers cannot forget it: any positive amount needs
    // the price bar; zero stays a plain body and never renders a price area.
    (void)display_type;
    const uint32_t derived_display_type = numeric_param > 0 ? 2 : 0;
    fn_popup_create_yesno(const_cast<char*>(text), length, 0, derived_display_type,
                          reinterpret_cast<void*>(ok), reinterpret_cast<void*>(cancel),
                          reinterpret_cast<void*>(static_cast<uintptr_t>(numeric_param)));
    return true;
}

bool ui_confirmation_popup_close_if_callbacks(UiPopupCallback expected_ok,
                                               UiPopupCallback expected_cancel,
                                               uint8_t* out_closed) {
    if (out_closed == nullptr || expected_ok == nullptr || expected_cancel == nullptr ||
        !main_thread_ready()) {
        return false;
    }
    *out_closed = 0;
    uint8_t active = 0;
    if (!ui_confirmation_popup_active(&active)) return false;
    if (active == 0) return true;
    UiPopupCallback current_ok = nullptr;
    UiPopupCallback current_cancel = nullptr;
    if (!ui_confirmation_popup_get_callbacks(&current_ok, &current_cancel)) return false;
    if (current_ok != expected_ok || current_cancel != expected_cancel) return true;
    if (!native_function_ready(reinterpret_cast<const void*>(fn_button_cancel_exe))) return false;
    fn_button_cancel_exe();
    active = 1;
    if (!ui_confirmation_popup_active(&active) || active != 0) return false;
    *out_closed = 1;
    return true;
}

bool ui_confirmation_popup_draw() {
    if (!main_thread_ready() ||
        !native_function_ready(reinterpret_cast<const void*>(fn_popup_draw))) {
        return false;
    }
    fn_popup_draw();
    return true;
}

}  // namespace qol::game

bool ui_save_lcd_now() {
    if (!qol::game::game_host_is_main_thread() || fn_grp_save_lcd == nullptr) return false;
    fn_grp_save_lcd();
    return true;
}
