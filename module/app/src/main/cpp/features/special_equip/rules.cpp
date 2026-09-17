#include "features/special_equip/rules.h"

namespace qol::features::special_equip {

bool equip_replay_allows(bool is_real_equip, bool can_change_equip, int char_level,
                         int item_level) {
    if (!is_real_equip) return false;     // ①
    if (!can_change_equip) return false;  // ③ ← 特殊 NPC 在此被拦
    // ④：原版用 `ldrb` 取角色等级（零扩展 u8）、`cmp` + `b.lt`（32 位有符号比较），此处照抄。
    return char_level >= item_level;
}

bool unequip_replay_allows(bool can_change_equip) {
    return can_change_equip;  // ② ← 特殊 NPC 在此被拦
}

bool sell_gate_decides_block(int desc_type, bool menu_char_present,
                             bool can_change_equip_resolved, bool can_change_equip) {
    if (desc_type != 0) return false;  // 仅装备详情（物品在角色身上）
    if (!menu_char_present) return false;
    if (!can_change_equip_resolved) return false;
    return !can_change_equip;
}

}  // namespace qol::features::special_equip
