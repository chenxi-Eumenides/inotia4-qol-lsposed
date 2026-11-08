#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace virtual_bag {

// 扩展袋编译期上限。不是引擎限制：袋号从 6 起、字段为 uint8_t，
// 理论上限为 249；这里取 128 是为内存/快照留出余量（State 约 0.55MB）。
// 实际可用数量由模块配置 extension_bag.bagCount 决定（默认未选择时回退页签 choice），UI 最多渲染 20 个。
constexpr int kMaxExtensionBags = 128;
constexpr int kOriginalTransactionBagCount = 5;
constexpr int kOriginalTaskBag = 5;
constexpr int kOriginalNativeBagCount = kOriginalTaskBag + 1;
constexpr int kExtensionLogicalBagFirst = 6;
constexpr int kSlotCount = 16;
constexpr int kRenderedExtensionBagTabsPerColumn = 5;
constexpr int kMaxRenderedExtensionBagTabs = 20;

#include "plugins/extension_bag/model/bag_catalog.inc"

constexpr int clamp_active_extension_bags(int count) {
    return count < 1 ? 1 : (count > kMaxExtensionBags ? kMaxExtensionBags : count);
}

constexpr int rendered_extension_bag_tabs(int active) {
    return active < 1 ? 1 : (active > kMaxRenderedExtensionBagTabs
        ? kMaxRenderedExtensionBagTabs : active);
}

constexpr int rendered_extension_bag_columns(int active) {
    return (rendered_extension_bag_tabs(active) + kRenderedExtensionBagTabsPerColumn - 1) /
           kRenderedExtensionBagTabsPerColumn;
}

constexpr int rendered_tab_column(int index) {
    return index < 0 ? 0 : index / kRenderedExtensionBagTabsPerColumn;
}

constexpr int rendered_tab_row(int index) {
    return index < 0 ? 0 : index % kRenderedExtensionBagTabsPerColumn;
}

inline int capacity_for_bag_type(int type) {
    return type >= 0 && type < static_cast<int>(kBagTypeCapacities.size())
               ? kBagTypeCapacities[static_cast<std::size_t>(type)]
               : 0;
}

int active_extension_bags();
int extension_logical_bag_last();

}  // namespace virtual_bag
