#pragma once

#include <cstdint>

#include "item.h"

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
// item 是背包物品本身的数据；capacity 是它提供的容量档位，接入 core.item 后由物品事实推导。
struct ItemBackpack {
    ItemData item{};
    ItemBackpackCapacity capacity = ItemBackpackCapacity::kNone;
};

}  // namespace qol::features::extra_bag::model
