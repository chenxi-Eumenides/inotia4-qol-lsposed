#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "bag.h"

namespace qol::features::extra_bag::model {

// 固定 5 个背包的内存容器；物品按 (bag_index, slot_index) 索引。
// 物品背包装备位不参与该索引。不做校验/拒绝；bag_index 越界时静默无操作。
// 不加锁；调用方自行保证线程安全。
class Store {
public:
    static constexpr uint32_t kBagCount = 5;

    Store();

    uint32_t bag_count() const;
    // 越界返回 nullptr。
    Bag* bag(uint32_t bag_index);
    const Bag* bag(uint32_t bag_index) const;

    void put(uint32_t bag_index, uint32_t slot_index, ItemData item);
    // 存在则移除并返回；不存在或 bag_index 越界返回 nullopt。
    std::optional<ItemData> take(uint32_t bag_index, uint32_t slot_index);
    // 不存在或 bag_index 越界返回 nullptr。
    const ItemData* item(uint32_t bag_index, uint32_t slot_index) const;

    // 清空并恢复 kBagCount 个空背包。
    void clear();

private:
    std::vector<Bag> bags_;
};

// 插件内单例。
Store& store();

}  // namespace qol::features::extra_bag::model
