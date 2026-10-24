#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace virtual_bag {

constexpr int kMaxExtensionBags = 8;
constexpr int kOriginalTransactionBagCount = 5;
constexpr int kOriginalTaskBag = 5;
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
