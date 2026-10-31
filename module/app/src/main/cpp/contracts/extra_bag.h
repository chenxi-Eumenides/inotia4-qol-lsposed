#pragma once

#include <cstdint>
#include <type_traits>

#include "version.h"

namespace qol::contracts {

// extra_bag 自有存储接口。payload 为游戏原生物品数据，插件之间直接传递，
// 本接口不解释 payload，也不提供使用/装备等游戏动作。
constexpr const char* kExtraBagCapabilityId = "extra_bag.storage";
constexpr QolVersion kExtraBagVersion{1, 1};
constexpr uint16_t kExtraBagVersionMajor = kExtraBagVersion.major;
constexpr uint16_t kExtraBagVersionMinor = kExtraBagVersion.minor;

constexpr uint32_t kExtraBagPayloadCapacity = 256;
constexpr uint32_t kExtraBagPayloadMaxSize = 255;

// 物品背包类型；容量映射为 0/4/8/12/16。
constexpr int32_t kExtraBagBackpackTypeNone = 0;
constexpr int32_t kExtraBagBackpackTypeSmall = 1;
constexpr int32_t kExtraBagBackpackTypeMedium = 2;
constexpr int32_t kExtraBagBackpackTypeLarge = 3;
constexpr int32_t kExtraBagBackpackTypeHuge = 4;

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

// 背包装备位：item 是物品背包本身，type 为 0/1/2/3/4。
struct QolExtraBagBackpack {
    QolExtraBagItem item{};
    int32_t type = kExtraBagBackpackTypeNone;
};

// 无回滚、无校验：put 直接覆盖目标位置；take/item_at 只报告是否存在。
// set_item_backpack 的 type=0 或 item=nullptr 表示清除装备位。
// 越界 bag/slot 按空处理；调用方负责 payload 格式与数量语义。
struct QolExtraBagApi {
    uint32_t struct_size = 0;
    int32_t (*bag_count)() = nullptr;
    void (*bag_info)(int32_t bag, QolExtraBagInfo* out) = nullptr;
    bool (*item_at)(int32_t bag, int32_t slot, QolExtraBagItem* out) = nullptr;
    void (*put)(int32_t bag, int32_t slot, const QolExtraBagItem* item) = nullptr;
    bool (*take)(int32_t bag, int32_t slot, QolExtraBagItem* out) = nullptr;
    // 1.1 尾部追加：背包装备位读取/设置/清除。
    bool (*item_backpack_at)(int32_t bag, QolExtraBagBackpack* out) = nullptr;
    void (*set_item_backpack)(int32_t bag, const QolExtraBagItem* item,
                              int32_t type) = nullptr;
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
static_assert(std::is_standard_layout<QolExtraBagBackpack>::value,
              "QolExtraBagBackpack must be standard layout");
static_assert(std::is_trivially_copyable<QolExtraBagBackpack>::value,
              "QolExtraBagBackpack must be trivially copyable");
static_assert(std::is_standard_layout<QolExtraBagApi>::value,
              "QolExtraBagApi must be standard layout");
static_assert(std::is_trivially_copyable<QolExtraBagApi>::value,
              "QolExtraBagApi must be trivially copyable");

}  // namespace qol::contracts
