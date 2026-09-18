#include "game/access/ui_kit.h"

#include "game_access.h"
#include "game_symbols.h"

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

}  // namespace


#include "game/access/ui_kit_controls.inc"
#include "game/access/ui_kit_render.inc"
