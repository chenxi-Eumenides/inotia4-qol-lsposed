#pragma once

#include <cstdint>

namespace qol::features::extension_bag {

constexpr uint32_t kNativeBagObjectMarkerShift = 25;
constexpr uint32_t kNativeBagObjectMarkerMask = 0x7Fu << kNativeBagObjectMarkerShift;

inline uint32_t write_native_bag_object_marker(uint32_t value) {
    return (value & ~kNativeBagObjectMarkerMask) |
           (1u << kNativeBagObjectMarkerShift);
}

}  // namespace qol::features::extension_bag
