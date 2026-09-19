#pragma once

#include <cstdint>

// 库存副作用动作能力。出售事务的 owner 是 inventory_hooks；消费者只通过此表
// 调用既有 inventory_trade::sell，不接触交易实现或游戏私有库存类型。

namespace qol::contracts {

constexpr const char* kInventoryActionCapabilityId = "inventory.action";
constexpr uint16_t kInventoryActionVersionMajor = 1;
constexpr uint16_t kInventoryActionVersionMinor = 0;

enum class InventorySellStatus : int32_t {
    kOk = 0,
    kNotInWorld = 1,
    kTaskBagExcluded = 2,
    kEmptySlot = 3,
    kNoNativeItem = 4,
    kStale = 5,
    kSymbolsUnresolved = 6,
    kNoSell = 7,
    kPriceInvalid = 8,
    kCreditFailed = 9,
    kRemoveFailed = 10,
};

struct InventorySellResult {
    InventorySellStatus status;
    int64_t price;
    int32_t category;
    int32_t count;
};

using InventorySellFn = InventorySellResult (*)(int32_t bag, int32_t slot,
                                                const void* expected_item,
                                                const char* source);

struct InventoryActionApi {
    InventorySellFn sell;
};

}  // namespace qol::contracts
