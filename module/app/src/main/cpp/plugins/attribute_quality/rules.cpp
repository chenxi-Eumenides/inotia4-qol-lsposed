#include "rules.h"

#include <climits>
#include <cstdint>

namespace attribute_quality {

Tier classify(int value, int min, int max) {
    // 退化区间（max <= min）无法计算百分位，统一视为满值。
    if (max <= min) {
        return Tier::Gold;
    }
    // 达到或超出上界（含特殊/固定值）视为满值。
    if (value >= max) {
        return Tier::Gold;
    }
    // 用 64 位避免 (value - min) * 100 在 int 下溢出；整数除法向下取整，
    // value < min 时 pct 为负数，自然落入 Grey。
    const int64_t width = static_cast<int64_t>(max) - min;
    const int64_t pct = (static_cast<int64_t>(value) - min) * 100 / width;
    if (pct >= 90) {
        return Tier::Purple;
    }
    if (pct >= 75) {
        return Tier::Blue;
    }
    if (pct >= 60) {
        return Tier::Green;
    }
    if (pct >= 30) {
        return Tier::White;
    }
    return Tier::Grey;
}

int percentile(int value, int min, int max) {
    // 退化区间（max <= min）无法计算百分位，统一视为满值。
    if (max <= min) {
        return 100;
    }
    // 达到或超出上界（含特殊/固定值）视为满值；低于下界为 0。
    if (value >= max) {
        return 100;
    }
    if (value < min) {
        return 0;
    }
    // 用 64 位避免 (value - min) * 100 在 int 下溢出；整数除法向下取整。
    // value 已在 [min, max) 内，结果落在 [0, 100)。
    const int64_t width = static_cast<int64_t>(max) - min;
    return static_cast<int>((static_cast<int64_t>(value) - min) * 100 / width);
}

QolResult evaluate_quality(int32_t value, qol::contracts::AttributeRange range,
                           const qol::contracts::AttributeQualityOptions* options,
                           qol::contracts::AttributeQuality* out) {
    if (out == nullptr) {
        static constexpr char kInvalidArgument[] = "invalid quality output";
        return qol::contracts::error(kQolErrInvalidArgument, kInvalidArgument,
                                     sizeof(kInvalidArgument) - 1);
    }
    *out = {};
    const uint8_t allow_outside = options == nullptr ? 0 : options->allow_outside_range;
    if (allow_outside > 1 ||
        (options != nullptr && (options->reserved[0] != 0 || options->reserved[1] != 0 ||
                                options->reserved[2] != 0))) {
        static constexpr char kInvalidArgument[] = "invalid quality options";
        return qol::contracts::error(kQolErrInvalidArgument, kInvalidArgument,
                                     sizeof(kInvalidArgument) - 1);
    }

    if (range.max <= range.min) {
        out->percentile = 100;
        out->level = qol::contracts::kAttributeQualityHighestLevel;
        return qol::contracts::ok();
    }

    const int64_t width = static_cast<int64_t>(range.max) - range.min;
    const int64_t numerator = (static_cast<int64_t>(value) - range.min) * 100;
    int64_t percentile = numerator / width;
    if (allow_outside == 0) {
        if (value < range.min) percentile = 0;
        if (value >= range.max) percentile = 100;
        if (percentile < 0) percentile = 0;
        if (percentile > 100) percentile = 100;
    } else if (percentile < INT32_MIN || percentile > INT32_MAX) {
        static constexpr char kPercentileOverflow[] = "percentile overflow";
        return qol::contracts::error(kQolErrInvalidArgument, kPercentileOverflow,
                                     sizeof(kPercentileOverflow) - 1);
    }

    out->percentile = static_cast<int32_t>(percentile);
    out->level = percentile >= 100 ? 5
                 : percentile >= 90 ? 4
                 : percentile >= 75 ? 3
                 : percentile >= 60 ? 2
                 : percentile >= 30 ? 1
                                    : 0;
    return qol::contracts::ok();
}

QolResult fixed_option_range(int32_t base, int32_t p1, int32_t p2,
                             int32_t* out_min, int32_t* out_max) {
    if (out_min == nullptr || out_max == nullptr) {
        static constexpr char kInvalidArgument[] = "invalid fixed option range output";
        return qol::contracts::error(kQolErrInvalidArgument, kInvalidArgument,
                                     sizeof(kInvalidArgument) - 1);
    }
    const int64_t safe_base = base > 0 ? base : 1;
    const int64_t minimum = safe_base + safe_base * p1 / 100;
    const int64_t maximum = safe_base + safe_base * p2 / 100;
    if (minimum < INT32_MIN || minimum > INT32_MAX ||
        maximum < INT32_MIN || maximum > INT32_MAX) {
        static constexpr char kRangeOverflow[] = "fixed option range overflow";
        return qol::contracts::error(kQolErrInvalidArgument, kRangeOverflow,
                                     sizeof(kRangeOverflow) - 1);
    }
    *out_min = static_cast<int32_t>(minimum);
    *out_max = static_cast<int32_t>(maximum);
    return qol::contracts::ok();
}

}  // namespace attribute_quality
