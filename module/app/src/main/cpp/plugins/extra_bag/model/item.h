#pragma once

#include <cstdint>

namespace qol::features::extra_bag::model {

// 插件持有的运行时物品对象；槽位只保存这一份数据。
// payload 为游戏原生物品数据格式，category/count 是接口层需要的元数据。
struct RuntimeItem {
    static constexpr uint32_t kPayloadCapacity = 256;
    static constexpr uint32_t kPayloadMaxSize = 255;

    int32_t category = 0;
    int32_t count = 0;
    uint32_t payload_size = 0;
    uint8_t payload[kPayloadCapacity] = {};
};

}  // namespace qol::features::extra_bag::model
