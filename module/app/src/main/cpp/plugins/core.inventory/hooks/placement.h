#pragma once

#include <cstdint>

#include "contracts/bag.h"

namespace qol::features::core_inventory::hooks::placement {

// 统一自动落槽顺序（全局严格七步）：
//   原生已有堆(space0) → 扩展已有堆(注册源注册序, space>=2) → C18 已有堆(space1)
//   → 同物品袋空槽(原生→注册源→C18，逐袋尝试) → 原生空槽(space0)
//   → 扩展空槽(注册序) → C18 空槽(space1)。
// 同物品袋空槽：袋内已有同身份物品但堆已满（或该类别不可堆叠）时，优先在该袋内新开堆；
// 第一个同身份袋满则继续下一个同身份袋，全部无处可放才进入原生空槽。身份判据与合并
// 一致（类别 + I_TYPE + 尾部身份区，排除 UID 与数量位）。
// 空间定义：原生 space0 bag0..4（任务袋 5 永不返回）；扩展 space>=2；C18 space1。
// C18 两步只在变体仍提供 C18 背包时参与（C18L v603 已移除，能力位关闭）；显式 scope
// 目标不受影响。落槽 bag 是空间内下标，写侧由调用方换算原生扁平袋号（6+bag）。
struct PlacementTarget {
    int32_t space_id;
    int32_t bag;
    int32_t slot;
    bool merge;  // true = 命中已有同身份堆（合并），false = 空槽
};

// scope_space < 0 = 全局自动落槽；scope_space >= 0 只扫该空间
// （空间内 merge → 同物品袋空槽 → empty）；scope_bag >= 0 只扫该 bag（要求
// scope_space >= 0，此时空槽扫描已限定在该袋，不再单独走同物品袋步骤）。
// merge 候选要求 stackable 且 category 相同且 payload 身份相同且堆叠预检不超上限；
// 同物品袋空槽对可堆叠与不可堆叠类别都生效。
bool resolve_placement(const contracts::QolBagTransfer& transfer, bool stackable,
                       int32_t scope_space, int32_t scope_bag, PlacementTarget* out);

}  // namespace qol::features::core_inventory::hooks::placement
