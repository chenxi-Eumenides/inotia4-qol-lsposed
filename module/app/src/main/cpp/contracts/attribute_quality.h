#pragma once

#include <cstdint>

#include "contracts/result.h"

// 属性质量只读能力（ARCHITECTURE.md §7.4 / ADR-0008）。
// provider 只返回范围、百分位与语义档位；游戏内联颜色码属于 display 消费者。

namespace qol::contracts {

constexpr const char* kAttributeQualityCapabilityId = "attribute.quality";
constexpr uint16_t kAttributeQualityVersionMajor = 1;
constexpr uint16_t kAttributeQualityVersionMinor = 0;

enum class AttributeQualityTier : uint8_t {
    Grey = 0,
    White = 1,
    Green = 2,
    Blue = 3,
    Purple = 4,
    Gold = 5,
};

struct AttributeQualityResult {
    int32_t value;
    int32_t min;
    int32_t max;
    int32_t percentile;
    AttributeQualityTier tier;
    uint8_t valid;
    uint16_t reserved;
};

using AttributeQualityProbeJewelFn = QolResult (*)(const void* item, int32_t attribute_type,
                                                    int32_t value, AttributeQualityResult* out);
using AttributeQualityProbeOptionFn = QolResult (*)(const void* item, int32_t option,
                                                     int32_t level, int32_t flag, int32_t value,
                                                     AttributeQualityResult* out);
using AttributeQualityClassifyFn = QolResult (*)(int32_t value, int32_t min, int32_t max,
                                                  AttributeQualityTier* out_tier);
using AttributeQualityPercentileFn = QolResult (*)(int32_t value, int32_t min, int32_t max,
                                                    int32_t* out_percentile);

struct AttributeQualityApi {
    AttributeQualityProbeJewelFn probe_jewel;
    AttributeQualityProbeOptionFn probe_option;
    AttributeQualityClassifyFn classify;
    AttributeQualityPercentileFn percentile;
};

}  // namespace qol::contracts
