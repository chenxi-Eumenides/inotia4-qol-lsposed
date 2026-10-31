#pragma once

// 袋源注册表骨架：只保存 BagSourceIo 指针，不拥有来源生命周期。
// 实现待补；当前未加入 CMake，不参与构建。

#include <cstdint>

#include "plugins/core.item/bags/bag_source_io.h"

namespace qol::features::core_item::orchestration {

class BagSourceRegistry {
public:
    BagSourceRegistry() = default;
    ~BagSourceRegistry() = default;

    BagSourceRegistry(const BagSourceRegistry&) = delete;
    BagSourceRegistry& operator=(const BagSourceRegistry&) = delete;

    // 主线程调用；space_id 重复或空指针返回 false。
    bool add(bags::BagSourceIo* source);
    bool remove(int32_t space_id);

    bags::BagSourceIo* find(int32_t space_id) const;
    bags::BagSourceIo* at(uint32_t index) const;
    uint32_t size() const;
    void clear();
};

}  // namespace qol::features::core_item::orchestration
