#pragma once

// 库存 payload 身份比较：与 extension_bag 的 mergeable_identity_equal 同源
// （plugins/extension_bag/model/virtual_bag_transaction_rules.inc），供
// core.inventory 落槽 resolver 与原生 move_handle 合并门共用同一判据。
// 只比较 I_TYPE 元数据 [9,11) 与数量字段之后的身份区 [15,size)（magic/socket/
// enchant/词缀链）；长度前缀、UID [1,9) 与数量位 [11,15) 是每实例可变字段，
// 不参与身份（整份 memcmp 永不相等是历史缺陷根因，禁止回退）。

#include <cstdint>
#include <cstring>

namespace qol::contracts {

inline bool qol_inventory_payload_identity_equal(const uint8_t* a, uint32_t a_size,
                                                 const uint8_t* b, uint32_t b_size) {
    constexpr uint32_t kTypeOffset = 9;
    constexpr uint32_t kTypeSize = 2;         // I_TYPE 位域字节数
    constexpr uint32_t kPostCountOffset = 15;  // 数量位 [11,15) 之后的身份区起点
    // 真实物品载荷最小长度（1 长度前缀 + 18 头，与 kPayloadHeaderSize 一致）；
    // 短于该长度无法构成完整身份区，一律不合并。
    constexpr uint32_t kMinPayloadSize = 19;
    if (a == nullptr || b == nullptr || a_size < kMinPayloadSize || b_size < kMinPayloadSize) {
        return false;
    }
    if (std::memcmp(a + kTypeOffset, b + kTypeOffset, kTypeSize) != 0) return false;
    const uint32_t tail = a_size - kPostCountOffset;
    if (tail != b_size - kPostCountOffset) return false;
    return tail == 0 || std::memcmp(a + kPostCountOffset, b + kPostCountOffset, tail) == 0;
}

}  // namespace qol::contracts
