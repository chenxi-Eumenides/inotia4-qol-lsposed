#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "item.h"
#include "item_backpack.h"

namespace qol::features::extra_bag::model {

// 内存背包模型：物品按 slot_index 存储，物品背包装备位独立且不参与该索引。
// 不做校验/拒绝：put 直接覆盖目标位置，不检查容量、slot 范围或物品数据。
// 不加锁；调用方自行保证线程安全。
class Bag {
public:
    static constexpr uint32_t kMaxCapacity = 16;

    // 未装备物品背包返回 0，否则返回其类型对应的容量（档位 0/4/8/12/16）。
    uint32_t capacity() const;
    bool has_item_backpack() const;
    const ItemBackpack* item_backpack() const;
    void equip_item_backpack(ItemBackpack backpack);
    void unequip_item_backpack();

    void put(uint32_t slot_index, RuntimeItem item);
    // 存在则移除并返回；不存在返回 nullopt。
    std::optional<RuntimeItem> take(uint32_t slot_index);
    // 不存在返回 nullptr。
    const RuntimeItem* item(uint32_t slot_index) const;
    uint32_t occupied_slots() const;

private:
    std::optional<ItemBackpack> item_backpack_;
    std::map<uint32_t, RuntimeItem> slots_;
};

}  // namespace qol::features::extra_bag::model
