// item_class.cpp —— 数据层物品类别原语（自 api/native/game_inventory_read.inc 下沉）。

#include "item_class.h"

#include "game_access.h"
#include "game_symbols.h"

#include <cstdint>

stack_codec::CountEncoding item_count_encoding(void* item) {
    if (g_base == 0 || item == nullptr) return stack_codec::CountEncoding::kUnknown;
    uint8_t* it = reinterpret_cast<uint8_t*>(item);
    const uint16_t flags = *reinterpret_cast<uint16_t*>(it + I_TYPE);
    uint8_t* class_data =
        *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    const uint8_t stride = size_ptr != nullptr ? *size_ptr : 0;
    return item_count_encoding_from_flags(flags, class_data, stride);
}

bool item_is_equip(void* item) {
    return item_count_encoding(item) == stack_codec::CountEncoding::kNotEncoded;
}

bool item_is_backpack(int category) {
    if (g_base == 0 || category < 0) return false;
    uint8_t* class_data =
        *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    const uint8_t stride = size_ptr != nullptr ? *size_ptr : 0;
    return item_is_backpack_from_class_data(category, class_data, stride);
}
