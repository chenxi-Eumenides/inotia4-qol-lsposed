#pragma once

#include <cstdint>
#include <type_traits>

#include "item_handle.h"

// 库存动作能力的唯一对外入口。所有物品副作用（移动、合并、使用、消耗、出售、
// 装备、镶嵌）都由 core.item 执行；调用方只提供显式袋引用，不接触游戏私有库存
// 类型，也不自行修改扩展袋数据。
//
// 线程约束：全部函数只在游戏主线程调用，不得跨线程持有 QolActionRef。

namespace qol::contracts {

constexpr const char* kInventoryActionCapabilityId = "inventory.action";
constexpr uint16_t kInventoryActionVersionMajor = 3;
constexpr uint16_t kInventoryActionVersionMinor = 0;

// 空间与袋号固定为调用时刻的显式值；core.item 执行前会用 item_at 复核句柄。
struct QolActionRef {
    int32_t space_id;
    int32_t bag;
    int32_t slot;
};

enum class InventoryActionStatus : int32_t {
    kOk = 0,
    kNotInWorld = 1,
    kInvalidRef = 2,
    kStale = 3,
    kEmpty = 4,
    kTaskBagExcluded = 5,
    kConflict = 6,
    kFull = 7,
    kUnsupported = 8,
    kInternal = 9,
    kCreditFailed = 10,
    kRollbackFailed = 11,
    kExtensionSaleUnavailable = 12,
};

// 结果状态沿用出售历史的取值，避免调用方按新枚举重排诊断。
enum class InventorySellStatus : int32_t {
    kOk = 0,
    kNotInWorld = 1,
    kTaskBagExcluded = 2,
    kEmptySlot = 3,
    kNoItemHandle = 4,
    kStale = 5,
    kSymbolsUnresolved = 6,
    kNoSell = 7,
    kPriceInvalid = 8,
    kCreditFailed = 9,
    kRemoveFailed = 10,
    kRollbackFailed = 11,
    kExtensionSaleUnavailable = 12,
};

struct InventoryMoveOutcome {
    InventoryActionStatus status;
    int32_t moved_count;
    uint8_t merged;  // 非 0 表示并入目标已有堆
    QolActionRef target;
    QolItemHandle target_item;
};

struct InventoryUseOutcome {
    InventoryActionStatus status;
    int32_t consumed_count;
};

struct InventorySellResult {
    InventorySellStatus status;
    int64_t price;
    int32_t category;
    int32_t count;
};

struct InventoryActionApi {
    uint32_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;

    // 同介质：移动或并入同类别堆；跨介质：执行转移（export→import→remove）。
    InventoryMoveOutcome (*move)(QolActionRef from, int32_t count, QolActionRef to);
    // 使用并结算可能的消耗。
    InventoryUseOutcome (*use)(QolActionRef ref);
    // 主动消耗/丢弃指定数量，不产生收益。
    InventoryUseOutcome (*discard)(QolActionRef ref, int32_t count);
    InventorySellResult (*sell)(QolActionRef ref, QolItemHandle expected_item, int32_t count,
                                const char* source);
    InventoryUseOutcome (*equip)(int32_t role, QolActionRef ref, int32_t equip_slot);
    InventoryUseOutcome (*unequip)(int32_t role, int32_t equip_slot);
    InventoryUseOutcome (*put_jewel)(int32_t role, QolActionRef jewel, int32_t equip_slot);
};

static_assert(std::is_standard_layout<QolActionRef>::value, "action ref must be standard layout");
static_assert(std::is_trivially_copyable<QolActionRef>::value, "action ref must be trivially copyable");
static_assert(std::is_standard_layout<InventoryActionApi>::value, "action api must be standard layout");
static_assert(std::is_trivially_copyable<InventoryActionApi>::value,
              "action api must be trivially copyable");

inline bool inventory_action_api_valid(const InventoryActionApi* api) {
    return api != nullptr && api->struct_size >= sizeof(InventoryActionApi) &&
           api->version_major == kInventoryActionVersionMajor &&
           api->version_minor >= kInventoryActionVersionMinor && api->move != nullptr &&
           api->use != nullptr && api->discard != nullptr && api->sell != nullptr &&
           api->equip != nullptr && api->unequip != nullptr && api->put_jewel != nullptr;
}

}  // namespace qol::contracts
