#pragma once

#include <cstdint>

#include "item.h"

namespace qol::features::extra_bag::model {

constexpr bool is_allowed_backpack_item_id(int32_t item_id) {
    return item_id >= 31 && item_id <= 34;
}

// 物品背包的 item_id 直接决定容量：
// 31=手提包->4，32=背包小->8，33=背包中->12，34=背包大->16。
constexpr uint32_t capacity_for_backpack_item_id(int32_t item_id) {
    switch (item_id) {
        case 31: return 4;
        case 32: return 8;
        case 33: return 12;
        case 34: return 16;
        default: return 0;
    }
}

// 物品背包：占用独立装备位，不属于普通容量。
// 物品本身即类型来源，不另存 type/capacity。
struct ItemBackpack {
    RuntimeItem item{};
};

}  // namespace qol::features::extra_bag::model
