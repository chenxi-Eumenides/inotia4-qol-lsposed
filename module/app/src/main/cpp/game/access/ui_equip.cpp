#include "game/access/ui_equip.h"

#include "game/symbols/game_symbols.h"
#include "game_access.h"

namespace qol::game {

bool ui_equip_move_merge_slot_target(void** out_slot, uintptr_t* out_expected) {
    if (g_base == 0 || out_slot == nullptr || out_expected == nullptr) return false;
    const uintptr_t offset = fn_resolve("F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA",
                                        F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA);
    if (offset == 0) return false;  // 符号未解析
    void* slot = reinterpret_cast<void*>(g_base + G_UIEQUIP_INVEN_ITEM_PROC_GOT_VMA);
    // 安装前守卫：槽不可读时不交给机制层尝试写入。
    if (!game_memory_accessible(slot, sizeof(void*), 'r')) return false;
    *out_slot = slot;
    *out_expected = g_base + offset;
    return true;
}

}  // namespace qol::game
