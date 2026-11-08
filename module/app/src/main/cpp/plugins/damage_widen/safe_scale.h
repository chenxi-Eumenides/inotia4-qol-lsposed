#pragma once

#include <cstdint>

namespace qol::features::damage_widen {

// Exact truncation-toward-zero equivalent of x * n / 1000 for the bounded int32
// game operands used by damage_widen. The decomposed products fit in int64.
inline int64_t safe_scale(int64_t x, int64_t n) {
    const int64_t quotient = x / 1000;
    const int64_t remainder = x % 1000;
    return quotient * n + (remainder * n) / 1000;
}

}  // namespace qol::features::damage_widen
