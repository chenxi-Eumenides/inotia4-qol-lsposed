#pragma once

#include <cstdint>
#include <vector>

#include "bag.h"

namespace qol::features::extra_bag::model {

// 多背包容器：背包数量由配置表示（set_bag_count，默认 5，不加上限）。
// 完全静态：无任何物品操作；clear 恢复默认 5 个空背包。不加锁。
class Store {
public:
    static constexpr uint32_t kDefaultBagCount = 5;

    // 默认创建 kDefaultBagCount 个空 Bag。
    Store();

    uint32_t bag_count() const;
    // 直接 resize，不加上限；缩小时多余背包丢弃。
    void set_bag_count(uint32_t count);
    // 越界返回 nullptr。
    Bag* bag(uint32_t index);
    const Bag* bag(uint32_t index) const;
    // 清空并恢复默认 kDefaultBagCount 个空背包（配置数量也回到默认）。
    void clear();

private:
    std::vector<Bag> bags_;
};

// 插件内单例。
Store& store();

}  // namespace qol::features::extra_bag::model
