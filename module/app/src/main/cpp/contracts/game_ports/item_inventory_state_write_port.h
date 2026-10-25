#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"
#include "../game_host.h"
#include "../item_handle.h"
#include "ports.h"
#include "item_port.h"

namespace qol::contracts {

constexpr uint32_t kQolItemInventoryStateGainedCapacity = 96;
constexpr uint32_t kQolItemInventoryStateHasGained = 1u << 0;

struct QolItemInventoryStateGainedItem {
    int32_t bag = -1;
    int32_t slot = -1;
    int32_t category = 0;
    int32_t count = 0;
};

struct QolItemInventoryStateWriteResult {
    int64_t value = 0;
    uint32_t flags = 0;
    uint32_t gained_count = 0;
    uint32_t error_message_len = 0;
    char error_message[128]{};
    QolItemInventoryStateGainedItem gained[kQolItemInventoryStateGainedCapacity]{};
};

using QolItemInventoryStateWriteFn = QolResult (*)(
    int32_t bag_or_slot, int32_t slot, QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateShopBuyFn = QolResult (*)(
    int32_t shop_slot, QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateAddItemFn = QolResult (*)(
    int32_t category, int32_t count, int32_t socket_total, int32_t enhance_remaining,
    int32_t rarity_group, const QolItemCountApi* item_count, QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateEquipFn = QolResult (*)(
    int32_t role, int32_t bag, int32_t slot, QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateUnequipFn = QolResult (*)(
    int32_t role, int32_t equip_slot, QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateApplyFn = QolResult (*)(
    int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
    QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateMoveFn = QolResult (*)(
    int32_t bag, int32_t slot, int32_t count, int32_t to_bag, int32_t to_slot,
    QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateLoadItemFn = QolResult (*)(
    const uint8_t* payload, uint32_t size, QolItemHandle* out_item);
using QolItemInventoryStatePlaceItemFn = QolResult (*)(
    const QolBagRef* ref, int32_t slot, QolItemHandle item,
    QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateRemoveItemFn = QolResult (*)(
    QolItemHandle item, int32_t count, QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateMoveHandleFn = QolResult (*)(
    QolItemHandle item, int32_t count, const QolBagRef* ref, int32_t slot,
    QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateEquipHandleFn = QolResult (*)(
    int32_t role, QolItemHandle item, int32_t equip_slot,
    QolItemInventoryStateWriteResult* out);
using QolItemInventoryStateUnequipHandleFn = QolResult (*)(
    int32_t role, int32_t equip_slot, QolItemInventoryStateWriteResult* out);
using QolItemInventoryStatePutJewelHandlesFn = QolResult (*)(
    int32_t role, QolItemHandle equip_item, QolItemHandle jewel_item,
    QolItemInventoryStateWriteResult* out);

struct QolItemInventoryStateWritePort {
    QolVersion abi_version;
    QolItemInventoryStateWriteFn use_item;
    QolItemInventoryStateWriteFn sell_item;
    QolItemInventoryStateWriteFn discard_item;
    QolItemInventoryStateShopBuyFn shop_buy;
    QolItemInventoryStateMoveFn move_item;
    QolItemInventoryStateAddItemFn add_item;
    QolItemInventoryStateEquipFn equip_item;
    QolItemInventoryStateUnequipFn unequip_item;
    QolItemInventoryStateApplyFn put_jewel;
    QolItemInventoryStateApplyFn enchant;
    QolItemInventoryStateLoadItemFn load_item;
    QolItemInventoryStatePlaceItemFn place_item;
    QolItemInventoryStateRemoveItemFn remove_item;
    QolItemInventoryStateMoveHandleFn move_handle;
    QolItemInventoryStateEquipHandleFn equip_handle;
    QolItemInventoryStateUnequipHandleFn unequip_handle;
    QolItemInventoryStatePutJewelHandlesFn put_jewel_handles;
};

// 1.2：尾部追加 SAVE_LoadItem、原生精确写槽与按句柄移除原语。
// load_item 返回调用方拥有的脱离背包对象句柄；成功 place_item 后对象归原生库存所有。
// Host 在 place_item 失败时释放 load_item 创建的对象，因此失败后句柄失效；成功后调用方不得释放。
// remove_item 失败时不改变槽位或数量；移除整堆会释放对象并使句柄失效。
// 2.0：尾部追加按句柄移动、装备、脱下与镶嵌原语；旧袋号写操作的 ABI 参数保持不变。
// move_handle 成功后，整堆放入空槽时句柄归原生库存；合并完全消耗对象时句柄失效；失败不转移所有权。
// equip_handle 成功转移 detached item 所有权，失败时对象仍归调用方；unequip_handle 成功后
// detached item 归调用方，调用方必须释放、丢弃或重新装备。
// put_jewel_handles 成功保留装备对象归属并消耗宝石对象；失败时两个对象归属均不变。
// 2.1：装备、脱下、镶嵌与附魔回调不再接收旧扩展库存 provider。
constexpr uint32_t kQolItemInventoryStateWritePortSize = sizeof(QolItemInventoryStateWritePort);

static_assert(std::is_standard_layout<QolItemInventoryStateGainedItem>::value,
              "QolItemInventoryStateGainedItem must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemInventoryStateGainedItem>::value,
              "QolItemInventoryStateGainedItem must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemInventoryStateWriteResult>::value,
              "QolItemInventoryStateWriteResult must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemInventoryStateWriteResult>::value,
              "QolItemInventoryStateWriteResult must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemInventoryStateWritePort>::value,
              "QolItemInventoryStateWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemInventoryStateWritePort>::value,
              "QolItemInventoryStateWritePort must remain trivially copyable");

}  // namespace qol::contracts
