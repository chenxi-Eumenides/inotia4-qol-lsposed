#pragma once

#include <cstdint>
#include <type_traits>

#include "game_ports/item_port.h"
#include "item_handle.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr QolVersion kQolInventoryExtensionVersion{2, 0};

struct QolInventoryExtensionItemRef {
    int32_t kind;
    int32_t bag;
    int32_t slot;
    int32_t category;
    int32_t count;
    QolItemHandle item;
};

using QolInventoryExtensionItemFn = int32_t (*)(const QolInventoryExtensionItemRef* item,
                                                 void* ctx);

struct QolInventoryExtensionProvider {
    int32_t (*owns_bag)(int32_t bag);
    QolResult (*item_ref_at)(int32_t bag, int32_t slot, QolInventoryExtensionItemRef* out);
    QolResult (*for_each_item)(QolInventoryExtensionItemFn fn, void* ctx);
    QolResult (*bag_capacity)(int32_t bag, int32_t* out_capacity);
    QolResult (*inventory_json_fragment)(char* out, uint32_t capacity, uint32_t* out_size);
    int32_t (*move_item)(int32_t from_bag, int32_t from_slot, int32_t to_bag,
                         int32_t to_slot, char* out, uint32_t capacity, uint32_t* out_size);
    QolResult (*sell_price)(QolItemHandle item, int32_t count, int32_t apply_discount,
                            int64_t* out_price);
    QolResult (*sync_projected_slot)(int32_t bag, int32_t slot);
    QolResult (*sync_projected_bag)();
    int32_t (*module_view_installed)();
    // 返回 1 且 out_bag 为公共逻辑袋号时命中 provider 物品；out_bag 由 provider 换算，
    // 调用方不自行推导袋号偏移。
    int32_t (*identify_native_item)(QolItemHandle item, int32_t* out_bag, int32_t* out_slot);
    int32_t (*remove_native_item)(QolItemHandle item);
    QolResult (*put_jewel)(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot);
    // 使用扩展逻辑袋物品：把原插件语义结果字符串（含信封）写入 out，返回 1 表示已回填。
    // 调用方按“无扩展可用”分支回退 typed 路径；本成员内部沿用插件启用门控。
    int32_t (*use_item)(int32_t bag, int32_t slot, char* out, uint32_t capacity,
                        uint32_t* out_size);
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
