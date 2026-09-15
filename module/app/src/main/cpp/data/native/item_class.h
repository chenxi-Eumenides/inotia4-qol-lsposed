#pragma once

#include <cstddef>
#include <cstdint>

#include "core/native/stack_codec.h"

// 数据层物品类别原语：ITEMCLASSBASE 记录 +6 bit0 的可堆叠/装备判定，
// 以及记录 +2 的背包类判定。从 api/native/game_inventory_read.inc 下沉
// （语义与实现零变更，仍读 G_ITEMCLASS_DATA / G_ITEMCLASS_SIZE）。

// 返回物品类别的计数编码；表基址/步长不可用或 item 为空返回 kUnknown，
// 调用方不得按可堆叠处理。
stack_codec::CountEncoding item_count_encoding(void* item);

bool item_is_equip(void* item);

// 背包类判定（自动出售阶段 B）：ITEMCLASSBASE 记录 +2 == 0x1f。
// 与扩展背包 category_is_extension_backpack 同口径；category 为 I_TYPE bit6-15。
bool item_is_backpack(int category);

// ITEMDATABASE/ITEMCLASSBASE 记录 +7 的「NPC 专属保护」位（bit4）：置位 = 不可装备 + 不可脱下
// （`CHAR_CanEquipItem @0xe4f3c` / `CHAR_CanUnequipItem @0xe4e80` 读的是同一位）。
constexpr uint8_t kNoEquipBit = 0x10;
constexpr size_t kNoEquipByteOffset = 7;

// 类别是否带该保护位。全表共 26 条置位（cat 485-506、785-787、948）。
bool category_is_no_equip(int category);

// 纯编码判定（可注入类别表，供 host 单测）：
//   category = I_TYPE bit6-15 解出的类别
//   class_data / stride 同 item_count_encoding_from_flags
// class_data 为空、stride==0 或 category<0 时 fail-closed 返回 false。
inline bool item_is_backpack_from_class_data(int category, const uint8_t* class_data,
                                             uint8_t stride) {
    if (class_data == nullptr || stride == 0 || category < 0) return false;
    return class_data[category * stride + 2] == 0x1f;
}

// 纯编码判定（可注入类别表，供 host 单测）：
//   class_data = ITEMCLASSBASE 表基址，stride = 每条记录步长
// class_data 为空、stride==0 或 category<0 时 fail-closed 返回 false。
inline bool category_is_no_equip_from_class_data(int category, const uint8_t* class_data,
                                                 uint8_t stride) {
    if (class_data == nullptr || stride == 0 || category < 0) return false;
    return (class_data[static_cast<size_t>(category) * stride + kNoEquipByteOffset] &
            kNoEquipBit) != 0;
}

// 纯编码判定（可注入类别表，供 host 单测）：
//   type_flags = I_TYPE 位域（bit6-15 = category）
//   class_data = ITEMCLASSBASE 表基址，stride = 每条记录步长
// class_data 为空或 stride==0 时 fail-closed 返回 kUnknown。
inline stack_codec::CountEncoding item_count_encoding_from_flags(uint16_t type_flags,
                                                                 const uint8_t* class_data,
                                                                 uint8_t stride) {
    if (class_data == nullptr || stride == 0) return stack_codec::CountEncoding::kUnknown;
    const int category = (type_flags >> 6) & 0x3FF;
    return (class_data[category * stride + 6] & 1) == 0
        ? stack_codec::CountEncoding::kNotEncoded
        : stack_codec::CountEncoding::kEncoded;
}
