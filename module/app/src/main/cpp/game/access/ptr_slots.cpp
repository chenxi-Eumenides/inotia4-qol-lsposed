#include "game/access/ptr_slots.h"

#include <cstring>

#include "game/access/save.h"
#include "game/access/qol_log_seam.h"
#include "game/access/ui_choice.h"
#include "game/access/ui_equip.h"
#include "game/symbols/game_symbols.h"
#include "game/access/game_access.h"

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

bool extension_desc_button_slot(uint32_t index, PtrSlotSite* out) {
    if (out == nullptr || index >= sizeof(UIEQUIP_DESC_BUTTON_SLOT_OFFSETS) /
                               sizeof(UIEQUIP_DESC_BUTTON_SLOT_OFFSETS[0]) ||
        g_base == 0) return false;
    auto* slot = reinterpret_cast<void**>(
        g_base + G_UIEQUIP_PANEL_VMA + UIEQUIP_DESC_BUTTON_SLOT_OFFSETS[index]);
    if (!game_memory_accessible(slot, sizeof(void*), 'r') || *slot == nullptr) return false;
    out->slot_addr = reinterpret_cast<uintptr_t>(slot);
    out->expected_value = *slot;
    return true;
}

bool extension_desc_button_instance(uint32_t index, void** out_button) {
    if (out_button == nullptr || index >= sizeof(UIEQUIP_DESC_BUTTON_SLOT_OFFSETS) /
                                      sizeof(UIEQUIP_DESC_BUTTON_SLOT_OFFSETS[0]) ||
        g_base == 0) {
        return false;
    }
    auto* button = reinterpret_cast<void**>(
        g_base + G_UIEQUIP_PANEL_VMA + UIEQUIP_DESC_BUTTON_SLOT_OFFSETS[index]);
    if (!game_memory_accessible(button, sizeof(void*), 'r')) return false;
    *out_button = *button;
    return *out_button != nullptr;
}

bool extension_desc_button_target(uint32_t index, DescButtonTarget* out) {
    if (out == nullptr || index >= sizeof(UIEQUIP_DESC_BUTTON_SLOT_OFFSETS) /
                                  sizeof(UIEQUIP_DESC_BUTTON_SLOT_OFFSETS[0]) ||
        g_base == 0) return false;
    *out = {};

    auto* panel_slot = reinterpret_cast<void**>(
        g_base + G_UIEQUIP_PANEL_VMA + UIEQUIP_DESC_BUTTON_SLOT_OFFSETS[index]);
    if (!game_memory_accessible(panel_slot, sizeof(void*), 'r')) return false;
    void* button = *panel_slot;
    if (button == nullptr ||
        !game_memory_accessible(static_cast<uint8_t*>(button) + CO_DATA, sizeof(void*), 'r'))
        return false;
    void* data = *reinterpret_cast<void**>(static_cast<uint8_t*>(button) + CO_DATA);
    if (data == nullptr) return false;
    void* execute_slot = static_cast<uint8_t*>(data) + CB_EXECUTE_PROC;
    // 面板槽位只在对应详情类型下由原版重建，其余时刻可能残留别的控件指针。这里要求
    // data 是可写的按钮私有块、槽内的原始 proc 是可执行代码，否则不是按钮：只查可读会
    // 把代码页当成按钮数据，后续写槽会破坏该页的取指权限（见 qol.log 2026-10-27 22:37）。
    if (!game_memory_accessible(execute_slot, sizeof(void*), 'w')) {
        QOL_LOG_SEAM_WARN(kQolLogDomainPlatform,
                          "desc button target rejected index=%u reason=data_not_writable button=%p data=%p",
                          static_cast<unsigned>(index), button, data);
        return false;
    }
    void* original_proc = *reinterpret_cast<void**>(execute_slot);
    if (original_proc == nullptr ||
        !game_memory_accessible(original_proc, 4, 'x')) {
        QOL_LOG_SEAM_WARN(kQolLogDomainPlatform,
                          "desc button target rejected index=%u reason=original_not_executable button=%p data=%p original=%p",
                          static_cast<unsigned>(index), button, data, original_proc);
        return false;
    }

    out->button = button;
    out->data = data;
    out->execute_slot = execute_slot;
    out->original_proc = original_proc;
    out->panel_slot_addr = reinterpret_cast<uintptr_t>(panel_slot);
    return true;
}

bool extension_desc_button_identity(void* button, void** out_data,
                                    void** out_execute_slot, void** out_current_proc) {
    if (out_data != nullptr) *out_data = nullptr;
    if (out_execute_slot != nullptr) *out_execute_slot = nullptr;
    if (out_current_proc != nullptr) *out_current_proc = nullptr;
    if (button == nullptr || out_data == nullptr || out_execute_slot == nullptr ||
        out_current_proc == nullptr ||
        !game_memory_accessible(static_cast<uint8_t*>(button) + CO_DATA, sizeof(void*), 'r'))
        return false;
    void* data = *reinterpret_cast<void**>(static_cast<uint8_t*>(button) + CO_DATA);
    if (data == nullptr) return false;
    void* execute_slot = static_cast<uint8_t*>(data) + CB_EXECUTE_PROC;
    if (execute_slot == nullptr ||
        !game_memory_accessible(execute_slot, sizeof(void*), 'r')) return false;
    void* current_proc = *reinterpret_cast<void**>(execute_slot);
    *out_data = data;
    *out_execute_slot = execute_slot;
    *out_current_proc = current_proc;
    return true;
}

bool extension_desc_button_original_matches(void* original, const char* symbol) {
    if (original == nullptr || symbol == nullptr || g_base == 0) return false;
    uintptr_t vma = 0;
    const char* resolved_symbol = symbol;
    if (std::strcmp(symbol, "desc.button.use") == 0 ||
        std::strcmp(symbol, "F_UIEQUIP_BUTTON_USE_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_USE_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_USE_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.use_after_confirm") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_USE_AFTER_CONFIRM_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_USE_AFTER_CONFIRM_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_USE_AFTER_CONFIRM_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.destroy") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_DESTROY_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_DESTROY_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_DESTROY_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.sell") == 0 ||
               std::strcmp(symbol, "F_UISTORE_BUTTON_SELL_EXE_VMA") == 0) {
        vma = F_UISTORE_BUTTON_SELL_EXE_VMA;
        resolved_symbol = "F_UISTORE_BUTTON_SELL_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.destroy_ok") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_OK_DESTROY_ITEM_VMA") == 0) {
        vma = F_UIEQUIP_OK_DESTROY_ITEM_VMA;
        resolved_symbol = "F_UIEQUIP_OK_DESTROY_ITEM_VMA";
    } else if (std::strcmp(symbol, "desc.button.equip") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_EQUIP_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_EQUIP_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_EQUIP_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.unequip") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.shortcut") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_SHORTCUT_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_SHORTCUT_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_SHORTCUT_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.remove_shortcut") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_REMOVE_SHORTCUT_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_REMOVE_SHORTCUT_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_REMOVE_SHORTCUT_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.use_mercenary_seal") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.release_sealed") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_RELEASE_SEALED_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_RELEASE_SEALED_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_RELEASE_SEALED_EXE_VMA";
    } else if (std::strcmp(symbol, "desc.button.roll_dice") == 0 ||
               std::strcmp(symbol, "F_UIEQUIP_BUTTON_ROLL_DICE_EXE_VMA") == 0) {
        vma = F_UIEQUIP_BUTTON_ROLL_DICE_EXE_VMA;
        resolved_symbol = "F_UIEQUIP_BUTTON_ROLL_DICE_EXE_VMA";
    } else {
        return false;
    }
    return original == reinterpret_cast<void*>(g_base + fn_resolve(resolved_symbol, vma));
}

}  // namespace qol::game
