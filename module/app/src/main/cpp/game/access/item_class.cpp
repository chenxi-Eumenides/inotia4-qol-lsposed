// item_class.cpp —— ITEMCLASS 表访问与装备判定链物品侧原语的 game 层收口。

#include "game/access/item_class.h"

#include "game_access.h"

#include <cstddef>

namespace qol::game {

void* item_class_record(uint16_t category) {
    if (g_base == 0) return nullptr;
    uint8_t* base = *reinterpret_cast<uint8_t**>(
        *reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (base == nullptr || size_ptr == nullptr) return nullptr;
    const uint8_t stride = *size_ptr;  // 记录长度（表长）；0 = 表未就绪
    if (stride == 0) return nullptr;
    return base + static_cast<size_t>(category) * stride;
}

bool item_class_no_equip_bit(const void* record) {
    if (record == nullptr) return false;
    const uint8_t* rec = static_cast<const uint8_t*>(record);
    return ((rec[7] >> 4) & 1) != 0;
}

int item_class_category(const void* item) {
    if (item == nullptr) return -1;
    const uint16_t flags =
        *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(item) + I_TYPE);
    return static_cast<int>((flags >> 6) & 0x3FF);
}

int equip_desc_role_type() {
    if (g_base == 0) return -1;
    return *reinterpret_cast<const uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
}

bool item_equip_primitives_ready() {
    return item_is_real_equip_resolved() && item_equip_level_resolved();
}

bool item_is_real_equip_resolved() {
    return fn_item_is_real_equip != nullptr;
}

bool item_equip_level_resolved() {
    return fn_item_get_equip_level != nullptr;
}

bool item_is_real_equip(const void* item) {
    return fn_item_is_real_equip != nullptr && item != nullptr &&
           fn_item_is_real_equip(const_cast<void*>(item)) != 0;
}

int item_equip_level(const void* item) {
    if (fn_item_get_equip_level == nullptr || item == nullptr) return 0;
    return fn_item_get_equip_level(const_cast<void*>(item));
}

}  // namespace qol::game
