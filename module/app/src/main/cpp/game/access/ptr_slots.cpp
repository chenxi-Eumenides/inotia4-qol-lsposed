#include "game/access/ptr_slots.h"

#include <cstring>

#include "game/access/save.h"
#include "game/access/ui_choice.h"
#include "game/access/ui_equip.h"
#include "game_access.h"

namespace qol::game {
namespace {

// 槽站点的「解析逻辑」由具体域提供（此处为存档域 save_slot_delete_hook_target），
// 本表只做登记名 → 解析器的映射；VMA/偏移常量仍收口在各域访问文件。
using Resolver = bool (*)(void** out_slot, uintptr_t* out_expected);

struct SlotEntry {
    const char* name;
    Resolver resolve;
};

constexpr SlotEntry kSlots[] = {
    // 游戏删档回调 SaveSlot_Delete 的 GOT 槽（内存档面板确认删档时经指针槽调用）。
    {"ptrslot:SAVESLOT_DELETE_GOT", &save_slot_delete_hook_target},
    // 原生选择框按钮 ExecuteProc 的 GOT 槽（模块截获选择索引以驱动业务回调）。
    {"ptrslot:g_uichoice_button_list_exe_got", &ui_choice_button_slot_target},
    // UIEquip 背包物品事件 proc 的 GOT 槽（move_merge 常驻指针槽，wrapper 内自门控）。
    {"ptrslot:MOVE_MERGE_GOT", &ui_equip_move_merge_slot_target},
};

}  // namespace

bool ptr_slot_site(const char* name, PtrSlotSite* out) {
    if (name == nullptr || out == nullptr) {
        return false;
    }
    for (const SlotEntry& entry : kSlots) {
        if (std::strcmp(entry.name, name) != 0) {
            continue;
        }
        if (entry.resolve == nullptr) {
            return false;
        }
        void* slot = nullptr;
        uintptr_t expected = 0;
        if (!entry.resolve(&slot, &expected)) {
            return false;
        }
        if (slot == nullptr || expected == 0) {
            return false;
        }
        // 与旧删档 PtrHook 的安装前守卫一致：槽不可读时不交给机制层尝试写入。
        if (!game_memory_accessible(slot, sizeof(void*), 'r')) {
            return false;
        }
        out->slot_addr = reinterpret_cast<uintptr_t>(slot);
        out->expected_value = reinterpret_cast<void*>(expected);
        return true;
    }
    return false;
}

}  // namespace qol::game
