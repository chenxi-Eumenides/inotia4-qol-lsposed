#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

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

struct QolItemInventoryStateWritePort {
    QolVersion abi_version;
    QolItemInventoryStateWriteFn use_item;
    QolItemInventoryStateWriteFn sell_item;
    QolItemInventoryStateWriteFn discard_item;
    QolItemInventoryStateShopBuyFn shop_buy;
};

constexpr QolVersion kQolPortItemInventoryStateWriteVersion{1, 0};

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
