#pragma once

// 跨空间物品转移契约（core.inventory 提供，core.item / extension_bag 消费）。
//
// 约束：
// - 所有回调只在游戏主线程调用；跨线程调用无锁保证。
// - move_item 只处理跨空间（原生 ↔ 注册源）或注册源之间的转移；
//   原生同空间（space 0/1 且 from.space_id == to.space_id）的原生移动路径归宿主
//   （QolItemPort move / QolMoveMergePort），本契约返回 kQolErrUnsupported，
//   文案固定为 "native move path owned by host"。
// - 任务袋（space 0, bag 5）只读，作为源或目标都被拒绝（kQolErrBlocked）。
// - 转移按 导出（只读）→ 目标导入 → 源移除 编排；失败不保证跨源回滚：
//   导入失败时源不动；源移除失败时尽力移除已导入的目标作为补偿，补偿失败
//   返回 kQolErrInternal（沿用 core.item move_item 的补偿语义）。
// - serialize_item 是 detached 句柄（load_item/create_item 产物）的序列化，
//   与原生导出同 payload 格式；非 detached 句柄由宿主序列化回调拒绝。
// - locate 按 space 分派：原生 space（0/1）走 item port 袋扫描，
//   注册源 space（>= kQolBagSpaceFirstRegistered）走 bag source identify。

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "bag.h"
#include "item_handle.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kInventoryTransferCapabilityId = "inventory.transfer";
constexpr QolVersion kInventoryTransferVersion{1, 0};

struct QolInventoryTransferApi {
    uint32_t struct_size;
    QolResult (*move_item)(const QolBagRef* from, int32_t from_slot, int32_t count,
                           const QolBagRef* to, int32_t to_slot,
                           QolBagMutationResult* out_target);
    QolResult (*serialize_item)(QolItemHandle item, int32_t count, QolBagTransfer* out);
    QolResult (*locate)(int32_t space, QolItemHandle item, QolBagRef* out, int32_t* out_slot);
};

inline bool qol_inventory_transfer_api_valid(const QolInventoryTransferApi* api) {
    constexpr uint32_t kRequiredSize =
        static_cast<uint32_t>(offsetof(QolInventoryTransferApi, locate) +
                              sizeof(decltype(QolInventoryTransferApi::locate)));
    return api != nullptr && api->struct_size >= kRequiredSize &&
           api->move_item != nullptr && api->serialize_item != nullptr &&
           api->locate != nullptr;
}

static_assert(std::is_standard_layout<QolInventoryTransferApi>::value,
              "QolInventoryTransferApi must remain standard layout");
static_assert(std::is_trivially_copyable<QolInventoryTransferApi>::value,
              "QolInventoryTransferApi must remain trivially copyable");

}  // namespace qol::contracts
