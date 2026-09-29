#pragma once

#include <cstdint>
#include <type_traits>

#include "game_ports/item_port.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr QolVersion kQolInventoryExtensionVersion{1, 0};

struct QolInventoryExtensionProvider {
    int32_t (*owns_bag)(int32_t bag);
    QolResult (*item_ref_at)(int32_t bag, int32_t slot, QolInventoryItemRef* out);
    QolResult (*for_each_item)(QolInventoryBagSlotFn fn, void* ctx);
    QolResult (*bag_capacity)(int32_t bag, int32_t* out_capacity);
    QolResult (*inventory_json_fragment)(char* out, uint32_t capacity, uint32_t* out_size);
    int32_t (*move_item)(int32_t from_bag, int32_t from_slot, int32_t to_bag,
                         int32_t to_slot, char* out, uint32_t capacity, uint32_t* out_size);
    QolResult (*sell_price)(const void* item, int32_t count, int32_t apply_discount,
                            int64_t* out_price);
    QolResult (*sync_projected_slot)(int32_t bag, int32_t slot);
    QolResult (*sync_projected_bag)();
    int32_t (*module_view_installed)();
    // 返回 1 且 out_bag 为公共逻辑袋号时命中 provider 物品；out_bag 由 provider 换算，
    // 调用方不自行推导袋号偏移。
    int32_t (*identify_native_item)(void* item, int32_t* out_bag, int32_t* out_slot);
    int32_t (*remove_native_item)(void* item);
    QolResult (*put_jewel)(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot);
};

struct QolInventoryExtensionAccess {
    const QolInventoryExtensionProvider* provider;
    int32_t resident;
    int32_t available;
};

static_assert(std::is_standard_layout<QolInventoryExtensionProvider>::value,
              "QolInventoryExtensionProvider must remain standard layout");
static_assert(std::is_trivially_copyable<QolInventoryExtensionProvider>::value,
              "QolInventoryExtensionProvider must remain trivially copyable");
static_assert(std::is_standard_layout<QolInventoryExtensionAccess>::value,
              "QolInventoryExtensionAccess must remain standard layout");
static_assert(std::is_trivially_copyable<QolInventoryExtensionAccess>::value,
              "QolInventoryExtensionAccess must remain trivially copyable");

}  // namespace qol::contracts
