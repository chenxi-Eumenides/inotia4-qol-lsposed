#pragma once

#include <cstdint>
#include <limits>

namespace sell_price {

constexpr int64_t kMaxValue = std::numeric_limits<int32_t>::max();

inline bool calculate(int64_t unit_price, uint32_t count, bool apply_variant_discount,
                      int64_t* out_price) {
    if (out_price == nullptr || unit_price < 0 || unit_price > kMaxValue || count == 0) {
        return false;
    }
    const int64_t multiplier = static_cast<int64_t>(count) *
                               (apply_variant_discount ? 7 : 10);
    if (unit_price > std::numeric_limits<int64_t>::max() / multiplier) return false;
    const int64_t final_price = (unit_price * multiplier) / 10;
    if (final_price < 0 || final_price > kMaxValue) return false;
    *out_price = final_price;
    return true;
}

}  // namespace sell_price
