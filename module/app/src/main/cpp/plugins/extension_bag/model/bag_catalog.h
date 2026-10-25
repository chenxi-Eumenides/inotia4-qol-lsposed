#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace virtual_bag {

// 扩展袋编译期上限。不是引擎限制：袋号从 6 起、字段为 uint8_t，
// 理论上限为 249；这里取 128 是为内存/快照留出余量（State 约 0.55MB）。
// 实际可用数量由 sidecar config.bagCount 决定（默认 5），页签当前只渲染前 5 个。
constexpr int kMaxExtensionBags = 128;
constexpr int kOriginalTransactionBagCount = 5;
constexpr int kOriginalTaskBag = 5;
constexpr int kOriginalNativeBagCount = kOriginalTaskBag + 1;
constexpr int kExtensionLogicalBagFirst = 6;
constexpr int kSlotCount = 16;
constexpr int kMaxRenderedExtensionBagTabs = 5;

#include "plugins/extension_bag/model/bag_catalog.inc"

constexpr int clamp_active_extension_bags(int count) {
    return count < 1 ? 1 : (count > kMaxExtensionBags ? kMaxExtensionBags : count);
}

inline int capacity_for_bag_type(int type) {
    return type >= 0 && type < static_cast<int>(kBagTypeCapacities.size())
               ? kBagTypeCapacities[static_cast<std::size_t>(type)]
               : 0;
}

int active_extension_bags();
int extension_logical_bag_last();

}  // namespace virtual_bag
