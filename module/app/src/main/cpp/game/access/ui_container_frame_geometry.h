#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "game/access/ui_kit.h"

namespace qol::game::ui_container_frame_geometry {

constexpr int32_t kCornerExtent = 24;
constexpr int32_t kFrameImageUnit = 0x0f;
constexpr int32_t kHorizontalLocId = 0x00;
constexpr int32_t kVerticalLocId = 0x01;
constexpr int32_t kFrameCornerLocIds[] = {0x0f, 0x10, 0x11, 0x12};

enum class Edge : uint8_t { kTop, kBottom, kLeft, kRight };
enum class Corner : uint8_t { kTopLeft, kTopRight, kBottomLeft, kBottomRight };

struct ImageLocation {
    int32_t source_x;
    int32_t source_y;
    int32_t width;
    int32_t height;
    int32_t offset_x;
    int32_t offset_y;
};

struct SourceRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct EdgeSourceProfile {
    int32_t tile_limit;
    int32_t decoration_extent;
};

struct ContainerFrameProfile {
    uint32_t image_unit;
    int32_t horizontal_loc;
    int32_t vertical_loc;
    int32_t corner_loc_ids[4];
    int32_t border_inset;
    int32_t straight_inner_inset;
    int32_t straight_decoration_thickness;
    EdgeSourceProfile horizontal;
    EdgeSourceProfile vertical;
    ImageLocation expected_horizontal;
    ImageLocation expected_vertical;
    ImageLocation expected_corners[4];
    const int32_t* corner_insets[4];
};

struct CornerSourceRun {
    int32_t source_x;
    int32_t source_y;
    int32_t source_width;
    int32_t source_height;
    int32_t destination_offset_x;
    int32_t destination_offset_y;
};

constexpr int32_t kCommonTopLeftInset[kCornerExtent] = {
    24, 24, 24, 24, 24, 24, 24, 24, 24, 23, 19, 17,
    15, 14, 13, 12, 12, 11, 11, 10, 10, 10, 10, 10,
};
constexpr int32_t kCommonTopRightInset[kCornerExtent] = {
    24, 24, 24, 24, 24, 24, 24, 24, 24, 23, 19, 17,
    15, 14, 13, 12, 12, 11, 11, 10, 10, 10, 10, 10,
};
constexpr int32_t kCommonBottomLeftInset[kCornerExtent] = {
    10, 10, 10, 10, 10, 11, 11, 12, 12, 13, 14, 15,
    17, 19, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24,
};
constexpr int32_t kCommonBottomRightInset[kCornerExtent] = {
    10, 10, 10, 10, 10, 11, 11, 12, 12, 13, 14, 15,
    17, 19, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24,
};

constexpr ContainerFrameProfile kCommonFrameProfile = {
    kFrameImageUnit,
    kHorizontalLocId,
    kVerticalLocId,
    {0x0f, 0x10, 0x11, 0x12},
    kCornerExtent,
    9,
    9,
    {508, 9},
    {383, 9},
    {2, 2, 508, 24, 0, 0},
    {2, 29, 24, 383, 0, 0},
    {
        {441, 179, 24, 24, 0, 0},
        {468, 179, 24, 24, 0, 0},
        {441, 206, 24, 24, 0, 0},
        {468, 206, 24, 24, 0, 0},
    },
    {kCommonTopLeftInset, kCommonTopRightInset,
     kCommonBottomLeftInset, kCommonBottomRightInset},
};

inline bool same_location(const ImageLocation& lhs, const ImageLocation& rhs) {
    return lhs.source_x == rhs.source_x && lhs.source_y == rhs.source_y &&
        lhs.width == rhs.width && lhs.height == rhs.height &&
        lhs.offset_x == rhs.offset_x && lhs.offset_y == rhs.offset_y;
}

inline bool valid_location(const ImageLocation& location) {
    return location.source_x >= 0 && location.source_y >= 0 && location.width > 0 &&
        location.height > 0 &&
        static_cast<int64_t>(location.source_x) + location.width <=
            std::numeric_limits<int32_t>::max() &&
        static_cast<int64_t>(location.source_y) + location.height <=
            std::numeric_limits<int32_t>::max();
}

inline bool fits_int32(int64_t value) {
    return value >= std::numeric_limits<int32_t>::min() &&
        value <= std::numeric_limits<int32_t>::max();
}

inline bool valid_rect(const ContainerFrameProfile& profile, UiRect rect) {
    constexpr int64_t max = std::numeric_limits<int32_t>::max();
    return profile.border_inset > 0 && rect.w >= 2LL * profile.border_inset &&
        rect.h >= 2LL * profile.border_inset && fits_int32(rect.x) && fits_int32(rect.y) &&
        rect.w <= max && rect.h <= max && rect.x <= max - rect.w && rect.y <= max - rect.h;
}

inline bool valid_rect(UiRect rect) {
    constexpr int64_t max = std::numeric_limits<int32_t>::max();
    return rect.w >= 2LL * kCornerExtent && rect.h >= 2LL * kCornerExtent &&
        fits_int32(rect.x) && fits_int32(rect.y) && rect.w <= max && rect.h <= max &&
        rect.x <= max - rect.w && rect.y <= max - rect.h;
}

inline bool matches_frame_profile(const ContainerFrameProfile& profile,
                                  const ImageLocation& horizontal,
                                  const ImageLocation& vertical,
                                  const ImageLocation (&corners)[4]) {
    if (!same_location(horizontal, profile.expected_horizontal) ||
        !same_location(vertical, profile.expected_vertical)) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!same_location(corners[i], profile.expected_corners[i])) return false;
    }
    return true;
}

inline const ContainerFrameProfile* select_frame_profile(
    const ImageLocation& horizontal, const ImageLocation& vertical,
    const ImageLocation (&corners)[4]) {
    return matches_frame_profile(kCommonFrameProfile, horizontal, vertical, corners)
        ? &kCommonFrameProfile
        : nullptr;
}

inline int32_t horizontal_source_extent(const ContainerFrameProfile& profile,
                                       const ImageLocation& location) {
    if (!valid_location(location) || location.height < profile.horizontal.decoration_extent) {
        return 0;
    }
    return std::min(location.width, profile.horizontal.tile_limit);
}

inline int32_t vertical_source_extent(const ContainerFrameProfile& profile,
                                     const ImageLocation& location) {
    if (!valid_location(location) || location.width < profile.vertical.decoration_extent) {
        return 0;
    }
    return std::min(location.height, profile.vertical.tile_limit);
}

inline int32_t bounded_tile_length(int64_t destination_length, int32_t available_source,
                                   int32_t native_limit) {
    if (destination_length <= 0 || available_source <= 0 || native_limit <= 0) return 0;
    return static_cast<int32_t>(std::min<int64_t>(
        destination_length, std::min(available_source, native_limit)));
}

inline bool horizontal_source_rect(const ContainerFrameProfile& profile,
                                  const ImageLocation& location, int32_t width,
                                  SourceRect* out) {
    if (out == nullptr || !valid_location(location) || width <= 0 ||
        width > horizontal_source_extent(profile, location)) {
        return false;
    }
    *out = {location.source_x, location.source_y, width,
            profile.horizontal.decoration_extent};
    return true;
}

inline bool vertical_source_rect(const ContainerFrameProfile& profile,
                                const ImageLocation& location, int32_t height,
                                SourceRect* out) {
    if (out == nullptr || !valid_location(location) || height <= 0 ||
        height > vertical_source_extent(profile, location)) {
        return false;
    }
    *out = {location.source_x, location.source_y,
            profile.vertical.decoration_extent, height};
    return true;
}

inline int64_t horizontal_start_x(const ContainerFrameProfile& profile, UiRect rect) {
    return rect.x + profile.border_inset;
}

inline int64_t bottom_edge_anchor_y(UiRect rect) {
    return rect.y + rect.h - 1;
}

inline int64_t vertical_start_y(const ContainerFrameProfile& profile, UiRect rect) {
    return rect.y + profile.border_inset;
}

inline int64_t right_edge_anchor_x(UiRect rect) {
    return rect.x + rect.w - 1;
}

inline bool edge_anchors_fit(int64_t start, int64_t length, int32_t tile_length,
                             int64_t source_offset) {
    if (length < 0 || source_offset < 0) return false;
    if (length == 0) return true;
    if (tile_length <= 0 || tile_length > length) return false;
    return fits_int32(start) && fits_int32(start + length - tile_length) &&
        fits_int32(start + length - 1);
}

inline int32_t native_flip_selector(Edge edge) {
    switch (edge) {
        case Edge::kBottom: return 1;
        case Edge::kRight: return 0;
        case Edge::kTop:
        case Edge::kLeft: return -1;
    }
    return -1;
}

inline int32_t corner_profile_inset(const ContainerFrameProfile& profile, Corner corner,
                                   int32_t row) {
    if (row < 0 || row >= profile.border_inset) return profile.border_inset;
    const size_t index = static_cast<size_t>(corner);
    if (index >= 4 || profile.corner_insets[index] == nullptr) return profile.border_inset;
    return profile.corner_insets[index][row];
}

inline bool corner_source_run(const ContainerFrameProfile& profile,
                              const ImageLocation& metadata, Corner corner,
                              int32_t first_row, int32_t row_count,
                              CornerSourceRun* out) {
    if (out == nullptr || !valid_location(metadata) || first_row < 0 || row_count <= 0 ||
        first_row + row_count > profile.border_inset ||
        metadata.width < profile.border_inset || metadata.height < profile.border_inset) {
        return false;
    }
    size_t index = 0;
    switch (corner) {
        case Corner::kTopLeft: index = 0; break;
        case Corner::kTopRight: index = 1; break;
        case Corner::kBottomLeft: index = 2; break;
        case Corner::kBottomRight: index = 3; break;
    }
    if (!same_location(metadata, profile.expected_corners[index])) return false;
    const int32_t inset = corner_profile_inset(profile, corner, first_row);
    for (int32_t row = first_row + 1; row < first_row + row_count; ++row) {
        if (corner_profile_inset(profile, corner, row) != inset) return false;
    }
    const bool right = corner == Corner::kTopRight || corner == Corner::kBottomRight;
    const int32_t source_x = right
        ? metadata.source_x + metadata.width - inset
        : metadata.source_x;
    const int32_t destination_x = right ? profile.border_inset - inset : 0;
    *out = {source_x, metadata.source_y + first_row, inset, row_count,
            destination_x, first_row};
    return true;
}

inline bool profile_fill_enabled(uint32_t alpha) {
    return alpha != 0;
}

inline bool profile_fill_row_span(const ContainerFrameProfile& profile, UiRect rect,
                                  int64_t row, UiRect* out) {
    if (out == nullptr || !valid_rect(profile, rect) || row < 0 || row >= rect.h ||
        row < profile.straight_decoration_thickness ||
        row >= rect.h - profile.straight_decoration_thickness) {
        return false;
    }
    int32_t left = profile.straight_inner_inset;
    int32_t right = profile.straight_inner_inset;
    if (row < profile.border_inset) {
        left = corner_profile_inset(profile, Corner::kTopLeft, static_cast<int32_t>(row));
        right = corner_profile_inset(profile, Corner::kTopRight, static_cast<int32_t>(row));
    } else if (row >= rect.h - profile.border_inset) {
        const int32_t corner_row = static_cast<int32_t>(row - (rect.h - profile.border_inset));
        left = corner_profile_inset(profile, Corner::kBottomLeft, corner_row);
        right = corner_profile_inset(profile, Corner::kBottomRight, corner_row);
    }
    const int64_t width = rect.w - left - right;
    if (width <= 0) return false;
    *out = {rect.x + left, rect.y + row, width, 1};
    return true;
}

inline bool profile_body_fill_rect(const ContainerFrameProfile& profile, UiRect rect,
                                   UiRect* out) {
    if (out == nullptr || !valid_rect(profile, rect)) return false;
    const int64_t width = rect.w - 2LL * profile.straight_inner_inset;
    const int64_t height = rect.h - 2LL * profile.border_inset;
    if (width <= 0 || height <= 0) return false;
    *out = {rect.x + profile.straight_inner_inset, rect.y + profile.border_inset,
            width, height};
    return true;
}

inline bool profile_fill_layout_valid(const ContainerFrameProfile& profile, UiRect rect) {
    if (!valid_rect(profile, rect) || profile.straight_decoration_thickness < 0 ||
        profile.straight_inner_inset < profile.straight_decoration_thickness ||
        profile.straight_inner_inset > profile.border_inset) {
        return false;
    }
    constexpr Corner kCorners[] = {
        Corner::kTopLeft, Corner::kTopRight, Corner::kBottomLeft, Corner::kBottomRight};
    for (int32_t row = 0; row < profile.border_inset; ++row) {
        for (Corner corner : kCorners) {
            const int32_t inset = corner_profile_inset(profile, corner, row);
            if (inset < profile.straight_inner_inset || inset > profile.border_inset) {
                return false;
            }
        }
    }
    return true;
}

inline bool profile_has_fill(const ContainerFrameProfile& profile, UiRect rect) {
    if (!profile_fill_layout_valid(profile, rect)) return false;
    for (int64_t row = profile.straight_decoration_thickness;
         row < profile.border_inset; ++row) {
        UiRect span{};
        if (profile_fill_row_span(profile, rect, row, &span)) return true;
    }
    const int64_t bottom_first = rect.h - profile.border_inset;
    const int64_t bottom_end = rect.h - profile.straight_decoration_thickness;
    for (int64_t row = bottom_first; row < bottom_end; ++row) {
        UiRect span{};
        if (profile_fill_row_span(profile, rect, row, &span)) return true;
    }
    UiRect body{};
    if (profile_body_fill_rect(profile, rect, &body)) return true;
    return false;
}

}  // namespace qol::game::ui_container_frame_geometry
