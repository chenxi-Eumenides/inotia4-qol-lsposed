#pragma once

#include <cstdint>

#include "contracts/bag.h"

namespace qol::features::core_inventory::hooks::placement {

// 统一自动落槽顺序（全局严格六步）：
//   原生已有堆(space0) → 扩展已有堆(注册源注册序, space>=2) → C18 已有堆(space1)
//   → 原生空槽(space0) → 扩展空槽(注册序) → C18 空槽(space1)。
// 空间定义：原生 space0 bag0..4（任务袋 5 永不返回）；扩展 space>=2；C18 space1。
// C18 落槽的 bag 是空间内下标，写侧由调用方换算原生扁平袋号（6+bag）。
struct PlacementTarget {
    int32_t space_id;
    int32_t bag;
    int32_t slot;
    bool merge;  // true = 命中已有同身份堆（合并），false = 空槽
};

// scope_space < 0 = 全局六步；scope_space >= 0 只扫该空间（空间内 merge → empty）；
// scope_bag >= 0 只扫该 bag（要求 scope_space >= 0）。
// merge 候选要求 stackable 且 category 相同且 payload 身份相同且堆叠预检不超上限。
bool resolve_placement(const contracts::QolBagTransfer& transfer, bool stackable,
                       int32_t scope_space, int32_t scope_bag, PlacementTarget* out);

}  // namespace qol::features::core_inventory::hooks::placement
