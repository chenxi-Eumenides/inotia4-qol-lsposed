#pragma once

#include <cstdint>
#include <optional>

#include "item_backpack.h"

namespace qol::features::extra_bag::model {

// 完全静态背包模型：只持有独立的物品背包装备位，无任何物品操作。
// 不做校验/拒绝：装备动作直接覆盖装备位。不加锁；调用方自行保证线程安全。
class Bag {
public:
    static constexpr uint32_t kMaxCapacity = 16;

    // 未装备物品背包返回 0，否则返回其容量枚举值转 uint32_t（档位 0/4/8/12/16）。
    uint32_t capacity() const;
    bool has_item_backpack() const;
    const ItemBackpack* item_backpack() const;
    void equip_item_backpack(ItemBackpack backpack);
    void unequip_item_backpack();

private:
    std::optional<ItemBackpack> item_backpack_;
};

}  // namespace qol::features::extra_bag::model
