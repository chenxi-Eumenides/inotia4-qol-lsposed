#pragma once

#include "plugins/extension_bag/model/bag_catalog.h"

#include <array>
#include <cstdint>

namespace qol::extension_bag {

// 轻量只读快照：只含标量/小数组字段，不含物品载荷。
// 由主线程在状态变更后发布，任意线程可读取，用于替代“读一个字段也加锁”。
struct BagSnapshot {
    std::array<uint8_t, virtual_bag::kMaxExtensionBags> types{};
    std::array<uint8_t, virtual_bag::kMaxExtensionBags> capacities{};
    int32_t mode = 0;
    int32_t selected = -1;
    int32_t inspected = -1;
    bool module_view_installed = false;
    int32_t module_view_index = -1;
    uint64_t revision = 0;
    uint64_t inventory_generation = 0;
};

struct BagProjectionSnapshot {
    bool module_view_installed = false;
    int32_t module_view_index = -1;
    uint64_t inventory_generation = 0;
};

// 主线程：把当前状态发布为最新快照（在 mark_dirty/提交点调用）。
void publish_bag_snapshot(const BagProjectionSnapshot& projection);
void publish_extension_bag_snapshot();

// 任意线程：读取最近一次已发布快照；尚未发布时返回 false。
bool bag_snapshot(BagSnapshot* out);

// 主线程：仅当 revision 变化或尚未发布时重建快照（每帧安全调用）。
void publish_bag_snapshot_if_changed(const BagProjectionSnapshot& projection);
void publish_extension_bag_snapshot_if_changed();

}  // namespace qol::extension_bag
