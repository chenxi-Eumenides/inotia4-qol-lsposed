#pragma once

#include <cstdint>

namespace stack_codec {

enum class CountEncoding : uint8_t {
    kUnknown,
    kNotEncoded,
    kEncoded,
};

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

// 原生袋对象 +0x10 的 bit0..24 是容量，bit25..31 才是对象 marker。
// 该字段禁止使用 write_count：普通物品数量位从 bit22 起，会覆盖袋容量。
constexpr uint32_t kNativeBagObjectMarkerShift = 25;
constexpr uint32_t kNativeBagObjectMarkerMask = 0x7Fu << kNativeBagObjectMarkerShift;

constexpr uint32_t write_native_bag_object_marker(uint32_t value) {
    return (value & ~kNativeBagObjectMarkerMask) |
           (1u << kNativeBagObjectMarkerShift);
}

}  // namespace stack_codec
