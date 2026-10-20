#include "rules.h"

namespace qol::features::special_equip {

bool equip_replay_allows(const EquipReplayInputs& in) {
    if (!in.is_real_equip) return false;     // ①
    if (!in.can_change_equip) return false;  // ③ ← 特殊 NPC 在此被拦
    // ④：原版用 `ldrb` 取角色等级（零扩展 u8）、`cmp` + `b.lt`（32 位有符号比较），此处照抄。
    return in.character_level >= in.required_item_level;
}

bool unequip_replay_allows(bool can_change_equip) {
    return can_change_equip;  // ② ← 特殊 NPC 在此被拦
}

bool sell_gate_decides_block(const SellGateInputs& in) {
    if (in.desc_type < 0 || in.desc_type > 2 || !in.menu_char_present ||
        !in.can_change_equip_resolved) {
        return true;
    }
    if (in.desc_type != 0) return false;  // 仅装备详情（物品在角色身上）
    return !in.can_change_equip;
}

}  // namespace qol::features::special_equip
