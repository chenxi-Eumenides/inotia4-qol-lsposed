#pragma once

#include <cstdint>
#include <type_traits>

#include "version.h"

namespace qol::contracts {

// extra_bag 自有存储接口。payload 为游戏原生物品数据，插件之间直接传递，
// 本接口不解释 payload，也不提供使用/装备等游戏动作。
constexpr const char* kExtraBagCapabilityId = "extra_bag.storage";
constexpr QolVersion kExtraBagVersion{1, 0};
constexpr uint16_t kExtraBagVersionMajor = kExtraBagVersion.major;
constexpr uint16_t kExtraBagVersionMinor = kExtraBagVersion.minor;

constexpr uint32_t kExtraBagPayloadCapacity = 256;
constexpr uint32_t kExtraBagPayloadMaxSize = 255;

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

// 无回滚、无校验：put 直接覆盖目标位置；take/item_at 只报告是否存在。
// 越界 bag/slot 按空处理；调用方负责 payload 格式与数量语义。
struct QolExtraBagApi {
    uint32_t struct_size = 0;
    int32_t (*bag_count)() = nullptr;
    void (*bag_info)(int32_t bag, QolExtraBagInfo* out) = nullptr;
    bool (*item_at)(int32_t bag, int32_t slot, QolExtraBagItem* out) = nullptr;
    void (*put)(int32_t bag, int32_t slot, const QolExtraBagItem* item) = nullptr;
    bool (*take)(int32_t bag, int32_t slot, QolExtraBagItem* out) = nullptr;
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
