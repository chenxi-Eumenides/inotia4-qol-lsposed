#pragma once

#include <cstdint>

#include "item.h"

namespace qol::features::extra_bag::model {

constexpr bool is_allowed_backpack_category(int32_t category) {
    return category >= 1 && category <= 4;
}

// 物品背包的 category 直接决定容量：
// 1=手提包->4，2=背包小->8，3=背包中->12，4=背包大->16。
constexpr uint32_t capacity_for_backpack_category(int32_t category) {
    switch (category) {
        case 1: return 4;
        case 2: return 8;
        case 3: return 12;
        case 4: return 16;
        default: return 0;
    }
}

// 物品背包：占用独立装备位，不属于普通容量。
// 物品本身即类型来源，不另存 type/capacity。
struct ItemBackpack {
    RuntimeItem item{};
};

}  // namespace qol::features::extra_bag::model
