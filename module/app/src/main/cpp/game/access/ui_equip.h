#pragma once

#include <cstdint>

// 游戏层：UIEquip 背包物品事件 proc 的指针槽站点（ARCHITECTURE.md §4.1 指针槽）。
// 与 game/access/save.h 的槽解析同构：只把「槽地址 + 安装前期望值」交给 patch 层，
// VMA/偏移保持 game_symbols.h 单一来源；机制实现不得在本层出现。

namespace qol::game {

// move_merge 指针槽：G_UIEQUIP_INVEN_ITEM_PROC_GOT_VMA 槽，期望值 =
// F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA。基址未就绪 / 符号未解析 / 槽不可读
// → false。
bool ui_equip_move_merge_slot_target(void** out_slot, uintptr_t* out_expected);

}  // namespace qol::game
