#pragma once

#include <cstdint>

namespace qol::features::extension_bag {

constexpr uint32_t kNativeBagObjectMarkerShift = 25;
constexpr uint32_t kNativeBagObjectMarkerMask = 0x7Fu << kNativeBagObjectMarkerShift;

// Native bag objects are non-encoded categories. The marker mask currently
// overlaps the S2 B segment, so callers must write only this marker and must
// not combine it with an item-count facade conversion.
inline uint32_t write_native_bag_object_marker(uint32_t value) {
    return (value & ~kNativeBagObjectMarkerMask) |
           (1u << kNativeBagObjectMarkerShift);
}

}  // namespace qol::features::extension_bag
