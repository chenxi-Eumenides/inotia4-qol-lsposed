#pragma once

// 袋源编排骨架：库存汇总与移动编排。实现待补，当前不参与构建。

#include <cstdint>

#include "plugins/core.item/bags/bag_source_io.h"
#include "plugins/core.item/bags/snapshot_store.h"
#include "plugins/core.item/orchestration/bag_source_registry.h"

namespace qol::features::core_item::orchestration {

// 汇总所有已注册来源为一份不可变快照；调用方负责发布。
QolResult build_inventory_snapshot(const BagSourceRegistry& registry,
                                   bags::InventorySnapshot* out);

// 同空间移动到来源自身的 move_within；跨空间按 export → import → remove 编排，
// 来源移除失败时删除目标补偿。失败不得报告成功。
QolResult move_item(const BagSourceRegistry& registry, const contracts::QolBagRef& from,
                    int32_t from_slot, int32_t count, const contracts::QolBagRef& to,
                    int32_t to_slot, contracts::QolBagMutationResult* out);

}  // namespace qol::features::core_item::orchestration
