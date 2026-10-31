#pragma once

#include <cstdint>

namespace qol::features::extra_bag::model {

// 普通物品数据。字段与 core.item 的 QolBagTransfer 对齐：
// category + canonical count + 固定 256B payload（有效长度 <=255）。
// QolItemHandle 是运行时身份，不属于持久化/存储数据，因此在接入袋源时另行物化。
struct ItemData {
    static constexpr uint32_t kPayloadCapacity = 256;
    static constexpr uint32_t kPayloadMaxSize = 255;

    int32_t category = 0;
    int32_t count = 0;
    uint32_t payload_size = 0;
    uint8_t payload[kPayloadCapacity] = {};
};

}  // namespace qol::features::extra_bag::model
