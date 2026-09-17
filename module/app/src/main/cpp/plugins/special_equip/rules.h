#pragma once

// 特殊装备解锁纯规则（无游戏依赖，host 可测）：原版装备判定链「跳过 bit4」后的重放判定，
// 以及装备详情出售按钮门禁决策。游戏值由 game/access 提供，本层只做组合判定。

namespace qol::features::special_equip {

// 原版 CHAR_CanEquipItem 判定链在「原版已返回 0」后跳过 ② 的重放：
//   ① ITEM_IsRealEquip(item)                             is_real_equip
//   ③ CHAR_CanChangeEquip(ch)                           can_change_equip
//   ④ ch[C_LEVEL] >= ITEM_GetEquipLevel(item)           char_level >= item_level
// 返回真精确等价于「原版只因 ②（+7 bit4）拒绝」。
bool equip_replay_allows(bool is_real_equip, bool can_change_equip, int char_level,
                         int item_level);

// 原版 CHAR_CanUnequipItem 跳过 ①（+7 bit4）后的检查：② CHAR_CanChangeEquip(ch)。
bool unequip_replay_allows(bool can_change_equip);

// 出售门禁决策：仅装备详情（desc_type==0）且菜单角色存在、装备操作原语已解析、
// 角色失去装备操作权限（特殊 NPC 佣兵）时拦截。desc_type<0（未就绪）、角色缺失或
// 原语未解析一律放行——fail-open 到原版行为，绝不误拦玩家背包物品。
bool sell_gate_decides_block(int desc_type, bool menu_char_present,
                             bool can_change_equip_resolved, bool can_change_equip);

}  // namespace qol::features::special_equip
