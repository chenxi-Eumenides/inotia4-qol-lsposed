#include "ui_shapes.h"

#include <algorithm>
#include <cstdint>

#include "ui_adapter_check.h"
#include "ui_geometry.h"

namespace qol::features::core_ui {
namespace {

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "ui primitive not ready";
// 与 ui_components.cpp 的错误文案长度保持一致：调用方只按 message_len 读取。
constexpr uint32_t kInvalidArgumentLength = 16;
constexpr uint32_t kNotReadyLength = 22;

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, kInvalidArgument, kInvalidArgumentLength);
}

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, kNotReady, kNotReadyLength);
}

constexpr uint32_t kMaxRasterRadius = 256;
constexpr uint32_t kMaxCircleDiameter = kMaxRasterRadius * 2;

bool valid_shape(const contracts::UiShapeDesc* desc) {
    return desc != nullptr && rect_within_int32(desc->rect) && desc->alpha <= 100 &&
        desc->fill_rgb565 <= 0xffffu && desc->border_rgb565 <= 0xffffu &&
        desc->border_width <= static_cast<uint64_t>(std::min(desc->rect.w, desc->rect.h) / 2);
}

QolResult shape_fill(const contracts::QolUiAdapter* adapter, UiRect rect, uint32_t color,
                     uint32_t alpha) {
    if (rect.w <= 0 || rect.h <= 0) return contracts::ok();
    if (adapter == nullptr || adapter->fill_rect_alpha == nullptr) return not_ready();
    return adapter->fill_rect_alpha(rect, color, alpha);
}

uint32_t integer_sqrt(uint64_t value, uint32_t upper_bound) {
    uint32_t low = 0;
    uint32_t high = upper_bound;
    while (low < high) {
        const uint32_t mid = low + (high - low + 1) / 2;
        if (static_cast<uint64_t>(mid) * mid <= value) low = mid;
        else high = mid - 1;
    }
    return low;
}

uint32_t rounded_corner_inset(uint32_t radius, uint32_t row) {
    // Work in doubled coordinates so the circle is evaluated at pixel centers. The first
    // included pixel satisfies 2*x+1 >= 2*r-sqrt((2*r)^2-dy^2).
    const int64_t dy = static_cast<int64_t>(2) * radius -
        (static_cast<int64_t>(2) * row + 1);
    const uint64_t doubled_radius = static_cast<uint64_t>(2) * radius;
    const uint64_t radius_squared = doubled_radius * doubled_radius;
    const uint64_t dy_squared = static_cast<uint64_t>(dy * dy);
    const uint32_t span = integer_sqrt(radius_squared - dy_squared,
                                       static_cast<uint32_t>(doubled_radius));
    return static_cast<uint32_t>((doubled_radius - span) / 2);
}

bool rounded_row_span(UiRect rect, uint32_t radius, int64_t row,
                      int64_t* out_x, int64_t* out_width) {
    if (out_x == nullptr || out_width == nullptr || row < 0 || row >= rect.h) return false;
    const int64_t from_edge = std::min(row, rect.h - 1 - row);
    const uint32_t inset = from_edge < radius
        ? rounded_corner_inset(radius, static_cast<uint32_t>(from_edge)) : 0;
    *out_x = rect.x + inset;
    *out_width = rect.w - static_cast<int64_t>(inset) * 2;
    return *out_width > 0;
}

bool circle_row_span(UiRect rect, int64_t row, int64_t* out_x, int64_t* out_width) {
    if (out_x == nullptr || out_width == nullptr || rect.w != rect.h || row < 0 ||
        row >= rect.h || rect.w > kMaxCircleDiameter) return false;
    const uint32_t diameter = static_cast<uint32_t>(rect.w);
    const int64_t doubled_diameter = diameter;
    const int64_t dy = static_cast<int64_t>(2) * row + 1 - doubled_diameter;
    const uint64_t radius_squared = static_cast<uint64_t>(diameter) * diameter;
    const uint64_t remaining = radius_squared - static_cast<uint64_t>(dy * dy);
    const int64_t dx = integer_sqrt(remaining, diameter);
    const int64_t left = std::max<int64_t>(0, (doubled_diameter - dx) / 2);
    const int64_t right = std::min<int64_t>(diameter - 1,
        (doubled_diameter + dx - 1) / 2);
    if (right < left) return false;
    *out_x = rect.x + left;
    *out_width = right - left + 1;
    return true;
}

QolResult draw_circle_pixels(const contracts::QolUiAdapter* adapter, UiRect rect, uint32_t color,
                             uint32_t alpha) {
    for (int64_t row = 0; row < rect.h; ++row) {
        int64_t x = 0, width = 0;
        if (!circle_row_span(rect, row, &x, &width)) continue;
        const QolResult result = shape_fill(adapter, {x, rect.y + row, width, 1}, color, alpha);
        if (!contracts::succeeded(result)) return result;
    }
    return contracts::ok();
}

}  // namespace

QolResult ui_shape_draw_rect(const contracts::QolUiAdapter* adapter,
                             const contracts::UiShapeDesc* desc) {
    if (!valid_shape(desc)) return invalid_argument();
    if (!ui_adapter_on_main_thread(adapter)) return not_ready();
    if (desc->border_width == 0) {
        return shape_fill(adapter, desc->rect, desc->fill_rgb565, desc->alpha);
    }
    const int64_t border = desc->border_width;
    const UiRect border_spans[] = {
        {desc->rect.x, desc->rect.y, desc->rect.w, border},
        {desc->rect.x, desc->rect.y + desc->rect.h - border, desc->rect.w, border},
        {desc->rect.x, desc->rect.y + border, border, desc->rect.h - 2 * border},
        {desc->rect.x + desc->rect.w - border, desc->rect.y + border,
         border, desc->rect.h - 2 * border}};
    for (const UiRect& span : border_spans) {
        const QolResult result = shape_fill(adapter, span, desc->border_rgb565, desc->alpha);
        if (!contracts::succeeded(result)) return result;
    }
    return shape_fill(adapter, {desc->rect.x + border, desc->rect.y + border,
                                desc->rect.w - 2 * border, desc->rect.h - 2 * border},
                      desc->fill_rgb565, desc->alpha);
}

QolResult ui_shape_draw_rounded_rect(const contracts::QolUiAdapter* adapter,
                                     const contracts::UiShapeDesc* desc) {
    if (!valid_shape(desc) || desc->radius > kMaxRasterRadius ||
        desc->radius > static_cast<uint64_t>(std::min(desc->rect.w, desc->rect.h) / 2)) {
        return invalid_argument();
    }
    if (!ui_adapter_on_main_thread(adapter)) return not_ready();
    if (desc->border_width == 0) {
        if (desc->radius == 0) {
            return shape_fill(adapter, desc->rect, desc->fill_rgb565, desc->alpha);
        }
        const int64_t middle_height = desc->rect.h - static_cast<int64_t>(desc->radius) * 2;
        if (middle_height > 0) {
            const QolResult result = shape_fill(adapter,
                {desc->rect.x, desc->rect.y + desc->radius, desc->rect.w, middle_height},
                desc->fill_rgb565, desc->alpha);
            if (!contracts::succeeded(result)) return result;
        }
        for (uint32_t row = 0; row < desc->radius; ++row) {
            const uint32_t inset = rounded_corner_inset(desc->radius, row);
            const int64_t width = desc->rect.w - static_cast<int64_t>(inset) * 2;
            QolResult result = shape_fill(adapter, {desc->rect.x + inset, desc->rect.y + row,
                width, 1}, desc->fill_rgb565, desc->alpha);
            if (!contracts::succeeded(result)) return result;
            result = shape_fill(adapter, {desc->rect.x + inset, desc->rect.y + desc->rect.h -
                row - 1, width, 1}, desc->fill_rgb565, desc->alpha);
            if (!contracts::succeeded(result)) return result;
        }
        return contracts::ok();
    }
    const int64_t inset = desc->border_width;
    const UiRect inner{desc->rect.x + inset, desc->rect.y + inset,
                       desc->rect.w - 2 * inset, desc->rect.h - 2 * inset};
    const uint32_t inner_radius = desc->radius > desc->border_width
        ? desc->radius - desc->border_width : 0;
    const int64_t radius = desc->radius;
    for (int64_t row = 0; row < radius; ++row) {
        for (int pass = 0; pass < 2; ++pass) {
            const int64_t y = pass == 0 ? row : desc->rect.h - row - 1;
            int64_t outer_x = 0, outer_w = 0;
            if (!rounded_row_span(desc->rect, desc->radius, y, &outer_x, &outer_w)) continue;
            int64_t fill_x = 0, fill_w = 0;
            const int64_t inner_y = y - inset;
            if (inner.w > 0 && inner.h > 0 &&
                rounded_row_span(inner, inner_radius, inner_y, &fill_x, &fill_w)) {
                const int64_t left_w = fill_x - outer_x;
                if (left_w > 0) {
                    const QolResult border_result = shape_fill(adapter,
                        {outer_x, desc->rect.y + y, left_w, 1},
                        desc->border_rgb565, desc->alpha);
                    if (!contracts::succeeded(border_result)) return border_result;
                }
                const QolResult fill_result = shape_fill(adapter,
                    {fill_x, desc->rect.y + y, fill_w, 1}, desc->fill_rgb565, desc->alpha);
                if (!contracts::succeeded(fill_result)) return fill_result;
                const int64_t right_x = fill_x + fill_w;
                const int64_t right_w = outer_x + outer_w - right_x;
                if (right_w > 0) {
                    const QolResult border_result = shape_fill(adapter,
                        {right_x, desc->rect.y + y, right_w, 1},
                        desc->border_rgb565, desc->alpha);
                    if (!contracts::succeeded(border_result)) return border_result;
                }
            } else {
                const QolResult border_result = shape_fill(adapter,
                    {outer_x, desc->rect.y + y, outer_w, 1}, desc->border_rgb565, desc->alpha);
                if (!contracts::succeeded(border_result)) return border_result;
            }
        }
    }
    const int64_t center_start = std::max<int64_t>(radius, inset);
    const int64_t center_end = desc->rect.h - center_start;
    const int64_t center_height = std::max<int64_t>(0, center_end - center_start);
    const int64_t border_band = center_start - radius;
    if (border_band > 0) {
        QolResult result = shape_fill(adapter, {desc->rect.x, desc->rect.y + radius,
            desc->rect.w, border_band}, desc->border_rgb565, desc->alpha);
        if (!contracts::succeeded(result)) return result;
        result = shape_fill(adapter, {desc->rect.x, desc->rect.y + center_end,
            desc->rect.w, border_band}, desc->border_rgb565, desc->alpha);
        if (!contracts::succeeded(result)) return result;
    }
    if (center_height > 0) {
        QolResult result = shape_fill(adapter, {desc->rect.x, desc->rect.y + center_start,
            inset, center_height}, desc->border_rgb565, desc->alpha);
        if (!contracts::succeeded(result)) return result;
        result = shape_fill(adapter, {desc->rect.x + desc->rect.w - inset,
            desc->rect.y + center_start, inset, center_height},
            desc->border_rgb565, desc->alpha);
        if (!contracts::succeeded(result)) return result;
        result = shape_fill(adapter, {inner.x, desc->rect.y + center_start,
            inner.w, center_height}, desc->fill_rgb565, desc->alpha);
        if (!contracts::succeeded(result)) return result;
    }
    return contracts::ok();
}

QolResult ui_shape_draw_circle(const contracts::QolUiAdapter* adapter,
                               const contracts::UiShapeDesc* desc) {
    if (!valid_shape(desc) || desc->rect.w != desc->rect.h ||
        desc->rect.w > kMaxCircleDiameter) return invalid_argument();
    if (!ui_adapter_on_main_thread(adapter)) return not_ready();
    if (desc->border_width == 0) {
        return draw_circle_pixels(adapter, desc->rect, desc->fill_rgb565, desc->alpha);
    }
    const int64_t inset = desc->border_width;
    const UiRect inner{desc->rect.x + inset, desc->rect.y + inset,
                       desc->rect.w - 2 * inset, desc->rect.h - 2 * inset};
    for (int64_t row = 0; row < desc->rect.h; ++row) {
        int64_t outer_x = 0, outer_w = 0;
        if (!circle_row_span(desc->rect, row, &outer_x, &outer_w)) continue;
        int64_t fill_x = 0, fill_w = 0;
        const int64_t inner_y = row - inset;
        if (inner.w > 0 && inner.h > 0 && circle_row_span(inner, inner_y, &fill_x, &fill_w)) {
            const int64_t left_w = fill_x - outer_x;
            if (left_w > 0) {
                const QolResult result = shape_fill(adapter, {outer_x, desc->rect.y + row,
                    left_w, 1}, desc->border_rgb565, desc->alpha);
                if (!contracts::succeeded(result)) return result;
            }
            QolResult result = shape_fill(adapter, {fill_x, desc->rect.y + row, fill_w, 1},
                                           desc->fill_rgb565, desc->alpha);
            if (!contracts::succeeded(result)) return result;
            const int64_t right_x = fill_x + fill_w;
            const int64_t right_w = outer_x + outer_w - right_x;
            if (right_w > 0) {
                result = shape_fill(adapter, {right_x, desc->rect.y + row, right_w, 1},
                    desc->border_rgb565, desc->alpha);
                if (!contracts::succeeded(result)) return result;
            }
        } else {
            const QolResult result = shape_fill(adapter, {outer_x, desc->rect.y + row,
                outer_w, 1}, desc->border_rgb565, desc->alpha);
            if (!contracts::succeeded(result)) return result;
        }
    }
    return contracts::ok();
}

}  // namespace qol::features::core_ui
