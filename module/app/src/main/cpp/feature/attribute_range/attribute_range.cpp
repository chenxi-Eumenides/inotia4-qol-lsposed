#include "feature/attribute_range/attribute_range.h"

#include <cstdint>

namespace attr_range {

char color_code(Tier tier) {
    switch (tier) {
        case Tier::Grey:
            return 'G';
        case Tier::White:
            return 'W';
        case Tier::Green:
            return 'C';
        case Tier::Blue:
            return 'L';
        case Tier::Purple:
            return 'V';
        case Tier::Gold:
            return 'Y';
    }
    return 'G';
}

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

}  // namespace attr_range
