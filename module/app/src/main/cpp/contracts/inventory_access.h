#pragma once

// 库存访问契约（core.inventory 提供给 core.item / extension_bag 的最小面）。
//
// 约束：
// - 所有回调只在游戏主线程调用；跨线程调用返回 kQolErrBlocked。
// - QolInventorySlotRef 是位置，QolInventoryItem.item 是同一会话内的身份。
// - 不做自动堆叠合并；place/move 只接受空目标槽。
// - 任务袋 (space 0, bag 5) 只读；写操作返回 kQolErrBlocked。
// - 失败不写槽：调用方在成功后自行决定是否继续下一步。
//
// 当前无生产消费者：仅 host test 直接使用；item/EB 仍走各自的旧存储面。
// 在 Phase B/C 接入前不要把本结构视为已冻结的生产 ABI；后续拆 query/store。

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "game_ports/item_port.h"
#include "item_handle.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kInventoryAccessCapabilityId = "inventory.access";
constexpr QolVersion kInventoryAccessVersion{1, 1};

struct QolInventorySlotRef {
    int32_t space_id;
    int32_t bag;
    int32_t slot;
};

// 槽内容的最小只读值；payload 不在层间流转。
struct QolInventoryItem {
    QolItemHandle item;
    int32_t category;
    int32_t count;
};

struct QolInventoryAccessApi {
    uint32_t struct_size;
    void* context;

    // 读取。
    QolResult (*item_at)(void* context, const QolInventorySlotRef* ref,
                         QolInventoryItemRef* out);
    QolResult (*locate)(void* context, QolItemHandle item, QolInventoryItemRef* out);
    QolResult (*find_first)(void* context, int32_t category, QolInventoryItemRef* out);
    QolResult (*find_empty)(void* context, int32_t space_id, QolInventorySlotRef* out);
    QolResult (*count_item)(void* context, int32_t category, int32_t* out_count);

    // 写入。move/take/place 处理整件物品；remove_count 处理部分数量。
    QolResult (*move)(void* context, const QolInventorySlotRef* from,
                      const QolInventorySlotRef* to);
    QolResult (*take)(void* context, const QolInventorySlotRef* ref, QolInventoryItem* out);
    QolResult (*place)(void* context, const QolInventorySlotRef* ref,
                       const QolInventoryItem* item);
    QolResult (*remove_count)(void* context, const QolInventorySlotRef* ref, int32_t count);

    // 1.1：原生空间只读语义（space 0 原版 / space 1 C18 追加袋）。无 context：
    // 由 core.inventory 组合模块的低层原语，消费者不再依赖 Item Port 的空间化接口。
    QolResult (*native_bag_count)(int32_t space_id, int32_t* out_count);
    QolResult (*native_bag_info)(const QolBagRef* ref, QolBagInfo* out_info);
    QolResult (*native_item_at)(const QolBagRef* ref, int32_t slot,
                                QolInventoryItemRef* out);
    QolResult (*native_for_each_space_slot)(int32_t space_id, QolInventoryBagSlotFn fn,
                                            void* ctx);
};

inline bool qol_inventory_access_api_valid(const QolInventoryAccessApi* api) {
    constexpr uint32_t kRequiredSize =
        static_cast<uint32_t>(offsetof(QolInventoryAccessApi, native_for_each_space_slot) +
                              sizeof(decltype(QolInventoryAccessApi::native_for_each_space_slot)));
    return api != nullptr && api->struct_size >= kRequiredSize &&
           api->item_at != nullptr && api->locate != nullptr && api->find_first != nullptr &&
           api->find_empty != nullptr && api->count_item != nullptr && api->move != nullptr &&
           api->take != nullptr && api->place != nullptr && api->remove_count != nullptr &&
           api->native_bag_count != nullptr && api->native_bag_info != nullptr &&
           api->native_item_at != nullptr && api->native_for_each_space_slot != nullptr;
}

// 只消费 1.1 原生空间方法的能力使用者（core.item/autosell）用此窄校验：
// 不要求 store 面成员非空，避免为了读原生空间而实现整套存写接口。
inline bool qol_inventory_native_access_api_valid(const QolInventoryAccessApi* api) {
    constexpr uint32_t kRequiredSize =
        static_cast<uint32_t>(offsetof(QolInventoryAccessApi, native_for_each_space_slot) +
                              sizeof(decltype(QolInventoryAccessApi::native_for_each_space_slot)));
    return api != nullptr && api->struct_size >= kRequiredSize &&
           api->native_bag_count != nullptr && api->native_bag_info != nullptr &&
           api->native_item_at != nullptr && api->native_for_each_space_slot != nullptr;
}

static_assert(std::is_standard_layout<QolInventorySlotRef>::value,
              "QolInventorySlotRef must remain standard layout");
static_assert(std::is_trivially_copyable<QolInventorySlotRef>::value,
              "QolInventorySlotRef must remain trivially copyable");
static_assert(std::is_standard_layout<QolInventoryItem>::value,
              "QolInventoryItem must remain standard layout");
static_assert(std::is_trivially_copyable<QolInventoryItem>::value,
              "QolInventoryItem must remain trivially copyable");
static_assert(std::is_standard_layout<QolInventoryAccessApi>::value,
              "QolInventoryAccessApi must remain standard layout");
static_assert(std::is_trivially_copyable<QolInventoryAccessApi>::value,
              "QolInventoryAccessApi must remain trivially copyable");

}  // namespace qol::contracts
