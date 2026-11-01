#pragma once

#include "contracts/bag.h"

namespace qol::features::core_inventory::transfer::move_item {

// 跨空间移动：导出（只读）→ 目标导入 → 源移除；失败时尽力补偿（移除已导入目标）。
// 原生同空间（space 0/1）路径归宿主，返回 kQolErrUnsupported。
QolResult move_item(const contracts::QolBagRef& from, int32_t from_slot, int32_t count,
                    const contracts::QolBagRef& to, int32_t to_slot,
                    contracts::QolBagMutationResult* out_target);

}  // namespace qol::features::core_inventory::transfer::move_item
