#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "item_handle.h"
#include "result.h"
#include "game_ports/item_port.h"

// 库存动作能力的唯一对外入口。所有物品副作用（移动、合并、使用、消耗、出售、
// 装备、镶嵌）都由 core.item 执行；调用方只提供显式袋引用，不接触游戏私有库存
// 类型，也不自行修改扩展袋数据。
//
// 线程约束：全部函数只在游戏主线程调用，不得跨线程持有 QolActionRef。

namespace qol::contracts {

constexpr const char* kInventoryActionCapabilityId = "inventory.action";
constexpr uint16_t kInventoryActionVersionMajor = 3;
constexpr uint16_t kInventoryActionVersionMinor = 3;

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

// 生成请求：payload 非空时按 SAVE_LoadItem 数据还原整件物品，否则按 category 新建。
// count <= 0 视为 1；count > 1 要求类别可堆叠。
struct QolItemCreateSpec {
    uint32_t struct_size;
    int32_t category;
    int32_t count;
    uint32_t flags;           // bit0 = perfect item（仅 category 路径）
    const uint8_t* payload;
    uint32_t payload_size;
};

constexpr uint32_t kQolItemCreateFlagPerfect = 1u << 0;

// 落位请求：原生空间（original/variant）直接写槽，注册源空间经序列化导入。
// ref.bag < 0 表示原生空间自动保存（INVEN_SaveItem 找槽并合并）；slot < 0 表示自动找槽。
struct QolItemPlaceTarget {
    uint32_t struct_size;
    QolBagRef ref;
    int32_t slot;
    uint32_t flags;           // bit0 = 允许合并/adopt
};

constexpr uint32_t kQolItemPlaceFlagMerge = 1u << 0;

struct QolItemCreateOutcome {
    uint32_t struct_size;
    InventoryActionStatus status;
    QolItemHandle item;       // 成功时返回调用方拥有的 detached 句柄
};

struct QolItemPlaceOutcome {
    uint32_t struct_size;
    InventoryActionStatus status;
    QolBagRef ref;            // 实际落位
    QolItemHandle item;       // 落位后句柄（注册源空间为导入后的新句柄）
    int32_t count;
    uint8_t merged;
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
    // 生成 detached 物品但不入库；调用方拥有句柄，负责 place 或 discard_handle。
    QolItemCreateOutcome (*create)(const QolItemCreateSpec* spec);
    // 把 detached 物品写入库存。注册源导入失败时句柄保持 detached；原生写槽沿用
    // place_item 的 Host 语义，失败时对象已由 Host 释放。
    QolItemPlaceOutcome (*place)(QolItemHandle item, const QolItemPlaceTarget* target);
    // 背包物品按数量移除；detached 句柄整体释放。
    InventoryUseOutcome (*discard_handle)(QolItemHandle item, int32_t count);
};

static_assert(std::is_standard_layout<QolActionRef>::value, "action ref must be standard layout");
static_assert(std::is_trivially_copyable<QolActionRef>::value, "action ref must be trivially copyable");
static_assert(std::is_standard_layout<QolItemCreateSpec>::value,
              "item create spec must be standard layout");
static_assert(std::is_trivially_copyable<QolItemCreateSpec>::value,
              "item create spec must be trivially copyable");
static_assert(std::is_standard_layout<QolItemPlaceTarget>::value,
              "item place target must be standard layout");
static_assert(std::is_trivially_copyable<QolItemPlaceTarget>::value,
              "item place target must be trivially copyable");
static_assert(std::is_standard_layout<QolItemCreateOutcome>::value,
              "item create outcome must be standard layout");
static_assert(std::is_trivially_copyable<QolItemCreateOutcome>::value,
              "item create outcome must be trivially copyable");
static_assert(std::is_standard_layout<QolItemPlaceOutcome>::value,
              "item place outcome must be standard layout");
static_assert(std::is_trivially_copyable<QolItemPlaceOutcome>::value,
              "item place outcome must be trivially copyable");
static_assert(std::is_standard_layout<InventoryActionApi>::value, "action api must be standard layout");
static_assert(std::is_trivially_copyable<InventoryActionApi>::value,
              "action api must be trivially copyable");
// v3.3 只在尾部追加；既有字段偏移不得变化，否则旧调用方按 struct_size 读取会错位。
static_assert(offsetof(InventoryActionApi, create) ==
                  offsetof(InventoryActionApi, put_jewel) +
                      sizeof(InventoryActionApi::put_jewel),
              "inventory action create offset changed");
static_assert(offsetof(InventoryActionApi, place) ==
                  offsetof(InventoryActionApi, create) + sizeof(InventoryActionApi::create),
              "inventory action place offset changed");
static_assert(offsetof(InventoryActionApi, discard_handle) ==
                  offsetof(InventoryActionApi, place) + sizeof(InventoryActionApi::place),
              "inventory action discard_handle offset changed");
static_assert(offsetof(InventoryActionApi, discard_handle) + sizeof(InventoryActionApi::discard_handle) ==
                  sizeof(InventoryActionApi),
              "inventory action tail is not the last field");

inline bool inventory_action_api_valid(const InventoryActionApi* api) {
    constexpr uint32_t kLegacySize = static_cast<uint32_t>(
        offsetof(InventoryActionApi, put_jewel) + sizeof(InventoryActionApi::put_jewel));
    if (api == nullptr || api->struct_size < kLegacySize ||
        api->version_major != kInventoryActionVersionMajor) {
        return false;
    }
    if (api->move == nullptr || api->use == nullptr || api->discard == nullptr ||
        api->sell == nullptr || api->equip == nullptr || api->unequip == nullptr ||
        api->put_jewel == nullptr) {
        return false;
    }
    // v3.3 尾部字段只在 struct_size 覆盖到时校验；旧调用方的表前缀仍然有效。
    constexpr uint32_t kCreateSize = static_cast<uint32_t>(
        offsetof(InventoryActionApi, create) + sizeof(InventoryActionApi::create));
    constexpr uint32_t kPlaceSize = static_cast<uint32_t>(
        offsetof(InventoryActionApi, place) + sizeof(InventoryActionApi::place));
    constexpr uint32_t kDiscardHandleSize = static_cast<uint32_t>(
        offsetof(InventoryActionApi, discard_handle) + sizeof(InventoryActionApi::discard_handle));
    if (api->struct_size >= kCreateSize && api->create == nullptr) return false;
    if (api->struct_size >= kPlaceSize && api->place == nullptr) return false;
    if (api->struct_size >= kDiscardHandleSize && api->discard_handle == nullptr) return false;
    return true;
}

}  // namespace qol::contracts
