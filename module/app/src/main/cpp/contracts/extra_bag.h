#pragma once

#include <cstdint>
#include <type_traits>

#include "version.h"

namespace qol::contracts {

// extra_bag 自有存储接口。payload 为游戏原生物品数据，插件之间直接传递，
// 本接口不解释 payload，也不提供使用/装备等游戏动作。
constexpr const char* kExtraBagCapabilityId = "extra_bag.storage";
constexpr QolVersion kExtraBagVersion{1, 3};
constexpr uint16_t kExtraBagVersionMajor = kExtraBagVersion.major;
constexpr uint16_t kExtraBagVersionMinor = kExtraBagVersion.minor;

constexpr uint32_t kExtraBagPayloadCapacity = 256;
constexpr uint32_t kExtraBagPayloadMaxSize = 255;

// 物品背包的 category 直接决定容量：
// 1=手提包->4，2=背包小->8，3=背包中->12，4=背包大->16。
// 其他 category 不是合法背包物品。
constexpr int32_t kExtraBagBackpackCategoryHandbag = 1;
constexpr int32_t kExtraBagBackpackCategorySmall = 2;
constexpr int32_t kExtraBagBackpackCategoryMedium = 3;
constexpr int32_t kExtraBagBackpackCategoryLarge = 4;

struct QolExtraBagItem {
    int32_t category = 0;
    int32_t count = 0;
    uint32_t payload_size = 0;
    uint8_t payload[kExtraBagPayloadCapacity] = {};
};

struct QolExtraBagInfo {
    int32_t capacity = 0;
    int32_t occupied_slots = 0;
};

// put/take 直接覆盖/移除，不做校验。
// set_item_backpack 仅接受 category 1..4，且装备位已有物品时返回 false；
// 需要先 clear_item_backpack 才能换装。越界 bag 同样返回 false。
struct QolExtraBagApi {
    uint32_t struct_size = 0;
    int32_t (*bag_count)() = nullptr;
    void (*bag_info)(int32_t bag, QolExtraBagInfo* out) = nullptr;
    bool (*item_at)(int32_t bag, int32_t slot, QolExtraBagItem* out) = nullptr;
    void (*put)(int32_t bag, int32_t slot, const QolExtraBagItem* item) = nullptr;
    bool (*take)(int32_t bag, int32_t slot, QolExtraBagItem* out) = nullptr;
    // 1.3：装备位读取/设置/清除；容量由 item.category 推导。
    bool (*item_backpack_at)(int32_t bag, QolExtraBagItem* out) = nullptr;
    bool (*set_item_backpack)(int32_t bag, const QolExtraBagItem* item) = nullptr;
    void (*clear_item_backpack)(int32_t bag) = nullptr;
};

static_assert(std::is_standard_layout<QolExtraBagItem>::value,
              "QolExtraBagItem must be standard layout");
static_assert(std::is_trivially_copyable<QolExtraBagItem>::value,
              "QolExtraBagItem must be trivially copyable");
static_assert(std::is_standard_layout<QolExtraBagInfo>::value,
              "QolExtraBagInfo must be standard layout");
static_assert(std::is_trivially_copyable<QolExtraBagInfo>::value,
              "QolExtraBagInfo must be trivially copyable");
static_assert(std::is_standard_layout<QolExtraBagApi>::value,
              "QolExtraBagApi must be standard layout");
static_assert(std::is_trivially_copyable<QolExtraBagApi>::value,
              "QolExtraBagApi must be trivially copyable");

}  // namespace qol::contracts
