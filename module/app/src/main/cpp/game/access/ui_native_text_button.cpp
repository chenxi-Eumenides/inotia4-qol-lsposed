#include "game/access/ui_native_text_button.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "game/access/game_access.h"
#include "game/adapter_ports/game_host_port.h"

namespace qol::game {
namespace {

constexpr int32_t kTextButtonImageUnit = 0x12;
constexpr int32_t kTextButtonImageLoc = 0x01;
constexpr int32_t kNativeToggleOnLoc = 0x0f;
constexpr int32_t kToggleBodyOffsetX = 13;
constexpr int32_t kToggleBodyOffsetY = 13;
// Visible toggle body target for the sliced capsule. Slice dest widths are
// src * percent / 100 and must all be integers; with the search restricted to even
// percents and the crop height fixed at 77, percent 70 is the only candidate whose
// height error stays within the 1.0 bound, and integer slice widths at 70% require
// cap and center source widths to be multiples of 10. The drawn width therefore only
// takes 7-logical-px steps: (2*100 + center) * 0.7 = 189 or 196 for this crop.
// 212 targets the 189 step: a 216 target would leave the best even-percent
// candidate (68%) at error 1.93 > 1.0 and abort the draw.
constexpr int32_t kToggleBodyWidth = 212;
constexpr int32_t kToggleBodyHeight = 61;
// Half of the 7-logical-px width reduction (3.5, floored): target_x moves right by
// this so the narrower capsule stays centered in the rect and the label stays
// within 1 logical px of the capsule center.
constexpr int32_t kToggleBodyCenterShiftX = 3;
// UI_006.png.jpg loc 0x01 gray capsule body; excludes transparent padding and side flourishes.
constexpr int32_t kTextButtonBodyOffsetX = 33;
constexpr int32_t kTextButtonBodyOffsetY = 22;
constexpr int32_t kTextButtonBodyWidth = 405;
constexpr int32_t kTextButtonBodyHeight = 77;
constexpr int32_t kTextButtonBodyCapWidth = 100;
// Plain-fill patch between the rounded caps. Must stay a multiple of 10 so its dest
// width stays integer at the percent values the search can select (70%).
constexpr int32_t kTextButtonBodyCenterWidth = 70;
constexpr int32_t kMaxScalePercent = 400;
constexpr int32_t kNativeDrawMode = 1;

struct ImageLayer {
    int32_t source_x;
    int32_t source_y;
    int32_t width;
    int32_t height;
    int32_t offset_x;
    int32_t offset_y;
};

struct ButtonGeometry {
    void* group = nullptr;
    ImageLayer layer{};
    int32_t percent = 0;
    int32_t image_x = 0;
    int32_t image_y = 0;
    int32_t draw_width = 0;
    int32_t cap_width = 0;
    int32_t cap_draw_width = 0;
    int32_t center_source_x = 0;
    int32_t center_source_width = 0;
    int32_t center_draw_width = 0;
    bool horizontal_slice = false;
};

bool read_layer(void* location, ImageLayer* out) {
    if (location == nullptr || out == nullptr || !game_memory_accessible(location, 0x18, 'r')) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(location);
    std::memcpy(&out->source_x, bytes + IMAGE_LOC_SOURCE_X, sizeof(out->source_x));
    std::memcpy(&out->source_y, bytes + IMAGE_LOC_SOURCE_Y, sizeof(out->source_y));
    std::memcpy(&out->width, bytes + IMAGE_LOC_SOURCE_WIDTH, sizeof(out->width));
    std::memcpy(&out->height, bytes + IMAGE_LOC_SOURCE_HEIGHT, sizeof(out->height));
    std::memcpy(&out->offset_x, bytes + IMAGE_LOC_OFFSET_X, sizeof(out->offset_x));
    std::memcpy(&out->offset_y, bytes + IMAGE_LOC_OFFSET_Y, sizeof(out->offset_y));
    return out->width > 0 && out->height > 0;
}

bool rect_fits_int32(UiRect rect) {
    const int64_t max = std::numeric_limits<int32_t>::max();
    if (rect.x < std::numeric_limits<int32_t>::min() || rect.x > max ||
        rect.y < std::numeric_limits<int32_t>::min() || rect.y > max || rect.w <= 0 ||
        rect.h <= 0 || rect.w > max || rect.h > max) return false;
    return rect.x <= max - rect.w && rect.y <= max - rect.h;
}

int64_t scale_round(int64_t value, int32_t percent) {
    return (value * percent + 50) / 100;
}

bool load_text_button_layer(void** out_group, ImageLayer* out_layer) {
    if (out_group == nullptr || out_layer == nullptr || fn_imgsys_unit_load == nullptr ||
        fn_imgsys_get_group == nullptr || fn_imgsys_get_loc == nullptr) return false;
    // Unit 0x12 is game-owned and shared. Ensure it is loaded; never unload it here.
    fn_imgsys_unit_load(kTextButtonImageUnit);
    *out_group = fn_imgsys_get_group(kTextButtonImageUnit);
    return *out_group != nullptr && game_memory_accessible(*out_group, sizeof(void*), 'r') &&
        read_layer(fn_imgsys_get_loc(kTextButtonImageUnit, kTextButtonImageLoc), out_layer);
}

bool load_native_toggle_layer(ImageLayer* out_layer) {
    if (out_layer == nullptr || fn_get_group_title_img_type == nullptr ||
        fn_imgsys_unit_load == nullptr || fn_imgsys_get_group == nullptr ||
        fn_imgsys_get_loc == nullptr) return false;
    const int32_t unit = fn_get_group_title_img_type();
    if (unit <= 0) return false;
    fn_imgsys_unit_load(unit);
    void* group = fn_imgsys_get_group(unit);
    if (group == nullptr || !game_memory_accessible(group, sizeof(void*), 'r')) return false;
    return read_layer(fn_imgsys_get_loc(unit, kNativeToggleOnLoc), out_layer);
}

bool text_button_geometry(UiRect rect, const ImageLayer& layer, int32_t* out_percent,
                          int32_t* out_x, int32_t* out_y) {
    if (out_percent == nullptr || out_x == nullptr || out_y == nullptr ||
        !rect_fits_int32(rect)) return false;
    const int64_t width_percent = rect.w * 100 / layer.width;
    const int64_t height_percent = rect.h * 100 / layer.height;
    const int64_t percent = width_percent < height_percent ? width_percent : height_percent;
    if (percent < 1 || percent > kMaxScalePercent) return false;
    const int64_t drawn_width = static_cast<int64_t>(layer.width) * percent / 100;
    const int64_t drawn_height = static_cast<int64_t>(layer.height) * percent / 100;
    const int64_t x = rect.x + (rect.w - drawn_width) / 2;
    const int64_t y = rect.y + (rect.h - drawn_height) / 2;
    if (x < std::numeric_limits<int32_t>::min() || x > std::numeric_limits<int32_t>::max() ||
        y < std::numeric_limits<int32_t>::min() || y > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *out_percent = static_cast<int32_t>(percent);
    *out_x = static_cast<int32_t>(x);
    *out_y = static_cast<int32_t>(y);
    return true;
}

bool load_text_button_geometry(UiRect rect, ButtonGeometry* out) {
    if (out == nullptr || !rect_fits_int32(rect) || !game_host_is_main_thread() ||
        fn_sgl_draw_texture_part_ex == nullptr) return false;
    ButtonGeometry geometry{};
    if (!load_text_button_layer(&geometry.group, &geometry.layer)) return false;
    const int64_t width_percent = rect.w * 100 / geometry.layer.width;
    const int64_t height_percent = rect.h * 100 / geometry.layer.height;
    if (height_percent <= width_percent ||
        (rect.w >= geometry.layer.width && rect.h >= geometry.layer.height)) {
        int32_t draw_x = 0;
        int32_t draw_y = 0;
        if (!text_button_geometry(rect, geometry.layer, &geometry.percent, &draw_x, &draw_y)) {
            return false;
        }
        geometry.image_x = draw_x;
        geometry.image_y = draw_y;
        geometry.draw_width = geometry.layer.width * geometry.percent / 100;
        *out = geometry;
        return true;
    }

    // Match the native toggle's actual source bounds, then crop the UI_006 pill to its opaque body.
    // Horizontal slicing preserves the rounded ends while shrinking only its plain center fill.
    geometry.horizontal_slice = true;
    ImageLayer toggle_layer{};
    if (!load_native_toggle_layer(&toggle_layer)) return false;
    const int64_t toggle_width_percent = rect.w * 100 / toggle_layer.width;
    const int64_t toggle_height_percent = rect.h * 100 / toggle_layer.height;
    const int64_t toggle_percent = toggle_width_percent < toggle_height_percent
        ? toggle_width_percent : toggle_height_percent;
    if (toggle_percent < 1 || toggle_percent > kMaxScalePercent) return false;
    const int64_t toggle_draw_width = static_cast<int64_t>(toggle_layer.width) *
        toggle_percent / 100;
    const int64_t toggle_draw_height = static_cast<int64_t>(toggle_layer.height) *
        toggle_percent / 100;
    const int64_t toggle_x = rect.x + (rect.w - toggle_draw_width) / 2;
    const int64_t toggle_y = rect.y + (rect.h - toggle_draw_height) / 2;
    const int64_t target_x = toggle_x + scale_round(
        static_cast<int64_t>(toggle_layer.offset_x) + kToggleBodyOffsetX, toggle_percent) +
        kToggleBodyCenterShiftX;
    const int64_t target_y = toggle_y + scale_round(
        static_cast<int64_t>(toggle_layer.offset_y) + kToggleBodyOffsetY, toggle_percent);
    const double target_width_exact =
        static_cast<double>(kToggleBodyWidth) * toggle_percent / 100.0;
    const double target_height_exact =
        static_cast<double>(kToggleBodyHeight) * toggle_percent / 100.0;
    const int64_t target_width = scale_round(kToggleBodyWidth, toggle_percent);
    const int64_t target_height = scale_round(kToggleBodyHeight, toggle_percent);
    if (target_x < rect.x || target_y < rect.y || target_x + target_width > rect.x + rect.w ||
        target_y + target_height > rect.y + rect.h || target_width <= 0 || target_height <= 0 ||
        target_x < INT32_MIN || target_x > INT32_MAX || target_y < INT32_MIN ||
        target_y > INT32_MAX) return false;

    // Include the full capsule contour plus neighboring black texels for filtered edge sampling.
    geometry.layer.source_x += kTextButtonBodyOffsetX;
    geometry.layer.source_y += kTextButtonBodyOffsetY;
    geometry.layer.width = kTextButtonBodyWidth;
    geometry.layer.height = kTextButtonBodyHeight;
    double best_error = std::numeric_limits<double>::max();
    int32_t body_percent = 0;
    for (int32_t candidate = 2; candidate <= kMaxScalePercent; candidate += 2) {
        const double candidate_width =
            static_cast<double>(kTextButtonBodyCapWidth * 2 + kTextButtonBodyCenterWidth) *
            candidate / 100.0;
        const double candidate_height =
            static_cast<double>(geometry.layer.height) * candidate / 100.0;
        const double width_error = std::abs(candidate_width - target_width_exact);
        const double height_error = std::abs(candidate_height - target_height_exact);
        const double error = width_error > height_error ? width_error : height_error;
        if (error < best_error) {
            best_error = error;
            body_percent = candidate;
        }
    }
    if (best_error > 1.0 || body_percent == 0) return false;
    geometry.percent = body_percent;
    geometry.cap_width = kTextButtonBodyCapWidth;
    geometry.cap_draw_width = geometry.cap_width * geometry.percent / 100;
    geometry.center_source_width = kTextButtonBodyCenterWidth;
    geometry.center_source_x = geometry.cap_width;
    geometry.center_draw_width = geometry.center_source_width * geometry.percent / 100;
    geometry.draw_width = geometry.cap_draw_width * 2 + geometry.center_draw_width;
    geometry.image_x = static_cast<int32_t>(target_x);
    geometry.image_y = static_cast<int32_t>(target_y);
    *out = geometry;
    return true;
}

bool valid_utf8(const char* text) {
    if (text == nullptr) return false;
    const size_t size = std::strlen(text);
    for (size_t pos = 0; pos < size;) {
        const auto lead = static_cast<uint8_t>(text[pos]);
        const size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                              (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 0;
        if (length == 0 || pos + length > size || (length == 2 && lead < 0xc2) ||
            (length == 4 && lead > 0xf4)) return false;
        for (size_t i = 1; i < length; ++i) {
            if ((static_cast<uint8_t>(text[pos + i]) & 0xc0) != 0x80) return false;
        }
        if (length == 3 && ((lead == 0xe0 && static_cast<uint8_t>(text[pos + 1]) < 0xa0) ||
                            (lead == 0xed && static_cast<uint8_t>(text[pos + 1]) >= 0xa0))) {
            return false;
        }
        if (length == 4 && ((lead == 0xf0 && static_cast<uint8_t>(text[pos + 1]) < 0x90) ||
                            (lead == 0xf4 && static_cast<uint8_t>(text[pos + 1]) > 0x8f))) {
            return false;
        }
        pos += length;
    }
    return true;
}

bool button_label_within_width(const char* text, int64_t max_width, int32_t font_height,
                               std::string* out) {
    if (text == nullptr || out == nullptr || max_width < font_height || font_height <= 0) {
        return false;
    }
    out->clear();
    const size_t size = std::strlen(text);
    size_t pos = 0;
    int64_t used = 0;
    bool truncated = false;
    while (pos < size) {
        const uint8_t lead = static_cast<uint8_t>(text[pos]);
        const size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                              (lead & 0xf0) == 0xe0 ? 3 : 4;
        if (used + font_height > max_width - font_height) {
            truncated = true;
            break;
        }
        out->append(text, pos, length);
        pos += length;
        used += font_height;
    }
    if (truncated) out->append("…");
    return !out->empty();
}

}  // namespace

bool ui_draw_native_text_button_background(UiRect rect, uint8_t pressed) {
    if (pressed > 1) return false;
    ButtonGeometry geometry{};
    if (!load_text_button_geometry(rect, &geometry)) return false;
    const auto draw_slice = [&geometry](int32_t x, int32_t source_x, int32_t source_width) {
        fn_sgl_draw_texture_part_ex(geometry.group, x, geometry.image_y,
            geometry.layer.source_x + source_x, geometry.layer.source_y, source_width,
            geometry.layer.height, geometry.layer.offset_x, geometry.layer.offset_y,
            kNativeDrawMode, 0, geometry.percent, 0);
    };
    if (!geometry.horizontal_slice) {
        draw_slice(geometry.image_x, 0, geometry.layer.width);
        return true;
    }
    const int32_t left_width = geometry.cap_draw_width;
    const int32_t center_width = geometry.center_draw_width;
    draw_slice(geometry.image_x, 0, geometry.cap_width);
    draw_slice(geometry.image_x + left_width, geometry.center_source_x,
               geometry.center_source_width);
    draw_slice(geometry.image_x + left_width + center_width,
               geometry.layer.width - geometry.cap_width, geometry.cap_width);
    return true;
}

bool ui_draw_native_text_button_label(UiRect rect, const char* text, uint32_t color, int32_t font) {
    if (!rect_fits_int32(rect) || !valid_utf8(text) || !game_host_is_main_thread() || font < 0 ||
        fn_grpx_get_font_height == nullptr || fn_grpx_set_font_color_rgb == nullptr ||
        fn_grpx_draw_string_with_font == nullptr) return false;
    const uint64_t measured_height = fn_grpx_get_font_height(static_cast<uint32_t>(font));
    if (measured_height == 0 || measured_height > static_cast<uint64_t>(INT32_MAX)) return false;

    ButtonGeometry geometry{};
    if (!load_text_button_geometry(rect, &geometry)) return false;
    const int32_t font_height = static_cast<int32_t>(measured_height);
    const int64_t image_height = scale_round(geometry.layer.height, geometry.percent);
    const int64_t content_left = geometry.image_x + geometry.draw_width * 11 / 100;
    const int64_t content_right = geometry.image_x + geometry.draw_width * 89 / 100;
    const int64_t content_top = geometry.image_y + image_height * 18 / 100;
    const int64_t content_bottom = geometry.image_y + image_height * 86 / 100;
    if (content_right <= content_left || content_bottom - content_top < font_height ||
        content_left < INT32_MIN || content_right > INT32_MAX || content_top < INT32_MIN ||
        content_bottom > INT32_MAX) return false;

    std::string label;
    if (!button_label_within_width(text, content_right - content_left, font_height, &label)) {
        return false;
    }
    const int64_t text_x = rect.x + rect.w / 2;
    const int64_t text_y = content_top + (content_bottom - content_top - font_height) / 2;
    if (text_x < INT32_MIN || text_x > INT32_MAX || text_y < INT32_MIN || text_y > INT32_MAX) {
        return false;
    }

    fn_grpx_set_font_color_rgb(static_cast<int32_t>(color & 0xff),
                               static_cast<int32_t>((color >> 8) & 0xff),
                               static_cast<int32_t>((color >> 16) & 0xff));
    fn_grpx_draw_string_with_font(label.data(), static_cast<int32_t>(text_x),
                                  static_cast<int32_t>(text_y), 2, font);
    return true;
}

}  // namespace qol::game
