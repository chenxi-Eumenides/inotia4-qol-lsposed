#include "game_ui_kit.h"

#include "game_access.h"
#include "game_symbols.h"

#include <cstring>

namespace {

constexpr int kPopupStateCount = 27;
constexpr size_t kPopupStateSize = 0x40;

void* popup_state_list() {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    return got == nullptr ? nullptr : *got;
}

uint8_t* find_popup_state(uintptr_t enter_vma) {
    uint8_t* list = static_cast<uint8_t*>(popup_state_list());
    if (list == nullptr) return nullptr;
    for (int i = 0; i < kPopupStateCount; ++i) {
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * kPopupStateSize + POPUP_ENTRY_ENTER);
        if (enter == g_base + enter_vma) return list + i * kPopupStateSize;
    }
    return nullptr;
}

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


#include "game_ui_kit_controls.inc"
#include "game_ui_kit_render.inc"
