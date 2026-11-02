#include "game/access/popup_state.h"

#include <cstring>

#include "game/symbols/game_symbols.h"
#include "game/access/game_access.h"

namespace qol::game {
namespace {

// state 表基址：GOT 槽 *(g_base + G_POPUP_STATE_LIST_GOT_VMA) → 27 条 × 0x40。
uint8_t* table_base() {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || !game_memory_accessible(got, sizeof(void*), 'r')) return nullptr;
    if (*got == nullptr) return nullptr;
    return reinterpret_cast<uint8_t*>(*got);
}

bool table_readable(const uint8_t* list) {
    return list != nullptr &&
           game_memory_accessible(list, POPUP_STATE_COUNT * POPUP_ENTRY_SIZE, 'r');
}

}  // namespace

bool popup_state_locate(uintptr_t enter_vma, PopupStateSlot* out) {
    if (out == nullptr || enter_vma == 0) return false;
    uint8_t* list = table_base();
    if (!table_readable(list)) return false;
    const uintptr_t target = g_base + enter_vma;
    for (size_t index = 0; index < POPUP_STATE_COUNT; ++index) {
        uint8_t* entry = list + index * POPUP_ENTRY_SIZE;
        if (*reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER) != target) continue;
        out->entry = entry;
        out->state_id = *reinterpret_cast<int32_t*>(entry);
        return true;
    }
    return false;
}

void popup_state_read(const PopupStateSlot& slot, uint8_t* out) {
    if (slot.entry == nullptr || out == nullptr) return;
    std::memcpy(out, slot.entry, POPUP_ENTRY_SIZE);
}

void popup_state_write_hooks(const PopupStateSlot& slot, const void* enter, const void* process,
                             const void* f3, const void* f4, const void* event) {
    if (slot.entry == nullptr) return;
    *reinterpret_cast<uintptr_t*>(slot.entry + POPUP_ENTRY_ENTER) =
        reinterpret_cast<uintptr_t>(enter);
    *reinterpret_cast<uintptr_t*>(slot.entry + POPUP_ENTRY_PROCESS) =
        reinterpret_cast<uintptr_t>(process);
    *reinterpret_cast<uintptr_t*>(slot.entry + POPUP_ENTRY_F3) = reinterpret_cast<uintptr_t>(f3);
    *reinterpret_cast<uintptr_t*>(slot.entry + POPUP_ENTRY_F4) = reinterpret_cast<uintptr_t>(f4);
    *reinterpret_cast<uintptr_t*>(slot.entry + POPUP_ENTRY_EVENT) =
        reinterpret_cast<uintptr_t>(event);
}

void popup_state_write_back(const PopupStateSlot& slot, const uint8_t* backup) {
    if (slot.entry == nullptr || backup == nullptr) return;
    std::memcpy(slot.entry, backup, POPUP_ENTRY_SIZE);
}

uintptr_t popup_state_backup_enter_vma(const uint8_t* backup) {
    if (backup == nullptr || g_base == 0) return 0;
    const uintptr_t original = *reinterpret_cast<const uintptr_t*>(backup + POPUP_ENTRY_ENTER);
    return original > g_base ? original - g_base : 0;
}

}  // namespace qol::game
