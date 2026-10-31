#pragma once

#include <cstdint>

#include "item.h"

namespace qol::features::extra_bag::model {

// 物品背包类型；容量映射为 0/4/8/12/16。
enum class ItemBackpackType : uint32_t {
    kNone = 0,
    kSmall = 1,
    kMedium = 2,
    kLarge = 3,
    kHuge = 4,
};

constexpr uint32_t capacity_for_item_backpack_type(ItemBackpackType type) {
    switch (type) {
        case ItemBackpackType::kSmall: return 4;
        case ItemBackpackType::kMedium: return 8;
        case ItemBackpackType::kLarge: return 12;
        case ItemBackpackType::kHuge: return 16;
        case ItemBackpackType::kNone: return 0;
    }
    return 0;
}

// 物品背包：占用独立装备位，不属于普通容量。
// item 是背包物品本身的数据；type 决定容量档位。
struct ItemBackpack {
    RuntimeItem item{};
    ItemBackpackType type = ItemBackpackType::kNone;
};

}  // namespace qol::features::extra_bag::model
