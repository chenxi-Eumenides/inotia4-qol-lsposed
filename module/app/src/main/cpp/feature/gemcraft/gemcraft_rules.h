#pragma once

namespace gemcraft {

// 宝石档位 category 判定：ITEMDATABASE 记录 28..32（28 低级 / 29 中级 / 30 高级 /
// 31 顶级 / 32 混沌）。等价于游戏 ITEMSYSTEM_IsJewel(0x10b964) 对宝石段的范围判定。
bool is_jewel_category(int category);

// 返回 filled[0..slot_count-1] 中索引最小的未填格；全部填满或 slot_count <= 0 返回 -1。
// filled 非空；slot_count 不得超过数组长度。
int first_empty_slot(const bool* filled, int slot_count);

}  // namespace gemcraft
