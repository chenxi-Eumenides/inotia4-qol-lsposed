#pragma once

#include <cstdint>

namespace stack_codec {

constexpr uint32_t kCountShift = 22;
constexpr uint32_t kCountBits = 10;
constexpr uint32_t kCountMask = ((1u << kCountBits) - 1u) << kCountShift;
constexpr uint32_t kLegacyMax = 99;
constexpr uint32_t kExtendedMax = 999;

constexpr uint32_t max_count(bool extended) {
    return extended ? kExtendedMax : kLegacyMax;
}

constexpr uint32_t clamp_count(uint32_t count, bool extended) {
    const uint32_t limit = max_count(extended);
    return count > limit ? limit : count;
}

constexpr uint32_t read_count(uint32_t value) {
    return (value & kCountMask) >> kCountShift;
}

constexpr uint32_t write_count(uint32_t value, uint32_t count) {
    return (value & ~kCountMask) | ((count << kCountShift) & kCountMask);
}

}  // namespace stack_codec
