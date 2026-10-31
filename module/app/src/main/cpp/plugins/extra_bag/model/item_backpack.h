#pragma once

#include <cstdint>

namespace qol::features::extra_bag::model {

// 物品背包可提供的实际容量；kNone 表示未装备。
enum class ItemBackpackCapacity : uint32_t {
    kNone = 0,
    kFour = 4,
    kEight = 8,
    kTwelve = 12,
    kSixteen = 16,
};

// 物品背包：占用独立装备位，不属于普通容量。
struct ItemBackpack {
    uint32_t item_id;
    ItemBackpackCapacity capacity;
};

}  // namespace qol::features::extra_bag::model
