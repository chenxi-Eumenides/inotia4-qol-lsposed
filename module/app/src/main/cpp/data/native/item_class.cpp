// item_class.cpp —— 数据层物品类别原语（自 api/native/game_inventory_read.inc 下沉）。

#include "item_class.h"

#include "contracts/item_count.h"
#include "game_access.h"
#include "game_symbols.h"

#include <cstdint>

qol::contracts::QolItemCountEncoding item_count_encoding(void* item) {
    if (item == nullptr) return qol::contracts::QolItemCountEncoding::kUnknown;
    const qol::contracts::QolItemCountApi* api = qol::contracts::item_count_api();
    if (api == nullptr || api->is_encoded == nullptr) {
        return qol::contracts::QolItemCountEncoding::kUnknown;
    }
    qol::contracts::QolItemCountEncoding encoding = qol::contracts::QolItemCountEncoding::kUnknown;
    const QolResult result = api->is_encoded(item, &encoding);
    return qol::contracts::succeeded(result) ? encoding : qol::contracts::QolItemCountEncoding::kUnknown;
}

bool item_is_equip(void* item) {
    return item_count_encoding(item) == qol::contracts::QolItemCountEncoding::kNotEncoded;
}

bool item_is_backpack(int category) {
    if (g_base == 0 || category < 0) return false;
    uint8_t* class_data =
        *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    const uint8_t stride = size_ptr != nullptr ? *size_ptr : 0;
    return item_is_backpack_from_class_data(category, class_data, stride);
}

bool category_is_no_equip(int category) {
    if (g_base == 0 || category < 0) return false;
    uint8_t* class_data =
        *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    const uint8_t stride = size_ptr != nullptr ? *size_ptr : 0;
    return category_is_no_equip_from_class_data(category, class_data, stride);
}
