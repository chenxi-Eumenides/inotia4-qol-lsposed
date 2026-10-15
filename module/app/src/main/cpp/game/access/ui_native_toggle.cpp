#include "game/access/ui_native_toggle.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "game/access/game_access.h"
#include "game/adapter_ports/game_host_port.h"

namespace qol::game {
namespace {

constexpr int32_t kToggleOnLoc = 0x0f;
constexpr int32_t kToggleOffLoc = 0x10;
constexpr int32_t kToggleDimLoc = 0x9;
constexpr int32_t kTogglePressedLoc = 0x7;
constexpr int32_t kMaxScalePercent = 400;
constexpr int32_t kNativeDrawMode = 1;

struct ToggleLayer {
    int32_t source_x;
    int32_t source_y;
    int32_t width;
    int32_t height;
    int32_t offset_x;
    int32_t offset_y;
};

bool read_layer(void* loc, ToggleLayer* out) {
    if (loc == nullptr || out == nullptr || !game_memory_accessible(loc, 0x18, 'r')) return false;
    const auto* bytes = static_cast<const uint8_t*>(loc);
    std::memcpy(&out->source_x, bytes + IMAGE_LOC_SOURCE_X, sizeof(out->source_x));
    std::memcpy(&out->source_y, bytes + IMAGE_LOC_SOURCE_Y, sizeof(out->source_y));
    std::memcpy(&out->width, bytes + IMAGE_LOC_SOURCE_WIDTH, sizeof(out->width));
    std::memcpy(&out->height, bytes + IMAGE_LOC_SOURCE_HEIGHT, sizeof(out->height));
    std::memcpy(&out->offset_x, bytes + IMAGE_LOC_OFFSET_X, sizeof(out->offset_x));
    std::memcpy(&out->offset_y, bytes + IMAGE_LOC_OFFSET_Y, sizeof(out->offset_y));
    return out->width > 0 && out->height > 0;
}

bool rect_fits_int32(UiRect rect) {
    constexpr int64_t min = std::numeric_limits<int32_t>::min();
    constexpr int64_t max = std::numeric_limits<int32_t>::max();
    if (rect.x < min || rect.x > max || rect.y < min || rect.y > max ||
        rect.w <= 0 || rect.h <= 0 || rect.w > max || rect.h > max) return false;
    return rect.x <= max - rect.w && rect.y <= max - rect.h;
}

}  // namespace

bool ui_draw_native_toggle(UiRect rect, int32_t value, uint8_t pressed) {
    if (value != 0 && value != 1) return false;
    if (pressed > 1 || !rect_fits_int32(rect) || !game_host_is_main_thread()) return false;
    if (fn_get_group_title_img_type == nullptr || fn_imgsys_unit_load == nullptr ||
        fn_imgsys_get_group == nullptr || fn_imgsys_get_loc == nullptr ||
        fn_sgl_draw_texture_part_ex == nullptr) return false;

    const int32_t image_unit = fn_get_group_title_img_type();
    if (image_unit <= 0) return false;
    fn_imgsys_unit_load(image_unit);
    void* group = fn_imgsys_get_group(image_unit);
    if (group == nullptr || !game_memory_accessible(group, sizeof(void*), 'r')) return false;

    const int32_t base_loc = value == 1 ? kToggleOnLoc : kToggleOffLoc;
    std::array<ToggleLayer, 3> layers{};
    std::array<int32_t, 3> loc_ids{};
    size_t layer_count = 1;
    loc_ids[0] = base_loc;
    if (value == 0) loc_ids[layer_count++] = kToggleDimLoc;
    if (pressed != 0) loc_ids[layer_count++] = kTogglePressedLoc;

    for (size_t i = 0; i < layer_count; ++i) {
        if (!read_layer(fn_imgsys_get_loc(image_unit, loc_ids[i]), &layers[i])) return false;
    }

    const ToggleLayer& base = layers[0];
    const int64_t width_percent = rect.w * 100 / base.width;
    const int64_t height_percent = rect.h * 100 / base.height;
    const int64_t percent = width_percent < height_percent ? width_percent : height_percent;
    if (percent < 1 || percent > kMaxScalePercent) return false;

    const int64_t drawn_width = static_cast<int64_t>(base.width) * percent / 100;
    const int64_t drawn_height = static_cast<int64_t>(base.height) * percent / 100;
    const int64_t x = rect.x + (rect.w - drawn_width) / 2;
    const int64_t y = rect.y + (rect.h - drawn_height) / 2;
    if (x < std::numeric_limits<int32_t>::min() || x > std::numeric_limits<int32_t>::max() ||
        y < std::numeric_limits<int32_t>::min() || y > std::numeric_limits<int32_t>::max()) {
        return false;
    }

    std::array<int32_t, 3> layer_xs{};
    std::array<int32_t, 3> layer_ys{};
    for (size_t i = 0; i < layer_count; ++i) {
        const ToggleLayer& layer = layers[i];
        // Monster v31 alpha>128 bodies are both 220x61: base [13,233)x[13,74),
        // loc 7 [21,241)x[21,82). Its extra 8 px per canvas edge is halo, not
        // a larger body to fit back into rect. Keep base scale and center only.
        const int64_t layer_x = x + (static_cast<int64_t>(base.width) - layer.width) * percent / 200;
        const int64_t layer_y = y + (static_cast<int64_t>(base.height) - layer.height) * percent / 200;
        if (layer_x < INT32_MIN || layer_x > INT32_MAX || layer_y < INT32_MIN ||
            layer_y > INT32_MAX) return false;
        layer_xs[i] = static_cast<int32_t>(layer_x);
        layer_ys[i] = static_cast<int32_t>(layer_y);
    }
    for (size_t i = 0; i < layer_count; ++i) {
        const ToggleLayer& layer = layers[i];
        fn_sgl_draw_texture_part_ex(group, layer_xs[i], layer_ys[i],
                                    layer.source_x, layer.source_y, layer.width, layer.height,
                                    base.offset_x, base.offset_y, kNativeDrawMode, 0,
                                    static_cast<int32_t>(percent), 0);
    }
    return true;
}

}  // namespace qol::game
