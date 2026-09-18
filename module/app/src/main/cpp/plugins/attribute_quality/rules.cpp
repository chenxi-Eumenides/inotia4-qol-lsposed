#include "plugins/attribute_quality/rules.h"

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
    const int64_t pct = (static_cast<int64_t>(value) - min) * 100 / (max - min);
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
    return static_cast<int>((static_cast<int64_t>(value) - min) * 100 / (max - min));
}

void fixed_option_range(int base, int p1, int p2, int* out_min, int* out_max) {
    const int safe_base = base > 0 ? base : 1;
    if (out_min != nullptr) {
        *out_min = safe_base + safe_base * p1 / 100;
    }
    if (out_max != nullptr) {
        *out_max = safe_base + safe_base * p2 / 100;
    }
}

}  // namespace attribute_quality
