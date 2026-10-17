#pragma once

#include <cstdint>
#include <limits>

namespace qol::contracts {

// 出售路径决定乘数来源：详情页销毁/出售按变体倍率，商店手动出售按原价。
enum QolSalePath : uint32_t { kDetailSell = 0, kStoreSell = 1 };

struct QolSaleQuote {
    uint32_t struct_size;
    int64_t unit_price;
    int32_t count;
    int64_t price;
    uint32_t multiplier_num;
    uint32_t multiplier_den;
};

}  // namespace qol::contracts

namespace qol::contracts::sell_price {

// 这是出售金额公式，不是商店或库存策略状态；调用方负责提供游戏事实（单价和数量）。
constexpr int64_t kMaxValue = std::numeric_limits<int32_t>::max();

inline bool calculate(int64_t unit_price, uint32_t count, uint32_t multiplier_num,
                      uint32_t multiplier_den, int64_t* out_price) {
    if (out_price == nullptr || unit_price < 0 || count == 0 || multiplier_den == 0 ||
        multiplier_num > multiplier_den) {
        return false;
    }

    const uint64_t scaled_count = static_cast<uint64_t>(count) * multiplier_num;
    const uint64_t whole_count = scaled_count / multiplier_den;
    const uint64_t remainder = scaled_count % multiplier_den;
    const uint64_t unit = static_cast<uint64_t>(unit_price);
    const uint64_t max_value = static_cast<uint64_t>(kMaxValue);

    if (whole_count != 0 && unit > max_value / whole_count) return false;
    const uint64_t whole_price = unit * whole_count;
    const uint64_t remaining = max_value - whole_price;

    const uint64_t unit_quotient = unit / multiplier_den;
    const uint64_t unit_remainder = unit % multiplier_den;
    if (remainder != 0 && unit_quotient > remaining / remainder) return false;
    const uint64_t fractional_whole = unit_quotient * remainder;
    const uint64_t fractional_part = (unit_remainder * remainder) / multiplier_den;
    if (fractional_part > remaining - fractional_whole) return false;

    *out_price = static_cast<int64_t>(whole_price + fractional_whole + fractional_part);
    return true;
}

[[deprecated("use the numerator/denominator overload")]]
inline bool calculate(int64_t unit_price, uint32_t count, bool apply_variant_discount,
                      int64_t* out_price) {
    return calculate(unit_price, count, apply_variant_discount ? 7u : 10u, 10u, out_price);
}

}  // namespace qol::contracts::sell_price
