#include "game/access/ui_native_close_button.h"

#include <cstdint>
#include <cstring>
#include <limits>

#include "game/access/game_access.h"
#include "game/adapter_ports/game_host_port.h"

namespace qol::game {
namespace {

constexpr int32_t kGlyphImageUnit = 0x0f;
constexpr int32_t kGlyphNormalLoc = 0x3a;
constexpr int32_t kGlyphPressedLoc = 0x34;
constexpr int32_t kFrameImageUnit = 0x59;
constexpr int32_t kFrameLoc = 0x3;
constexpr int32_t kNativeDrawMode = 1;
constexpr int32_t kNativeScalePercent = 100;

// 原生 X/返回共用的金属框盒尺寸（0x59 loc3 的自然尺寸）：两者在同一盒内只差字形与按下光效。
constexpr int64_t kFrameWidth = 78;
constexpr int64_t kFrameHeight = 86;
// 原生 X 字形 1:1 摆放：loc0x3a @ rect+(15,24)，按下底板 loc0x34 @ rect+(3,12)。
constexpr int64_t kGlyphOffsetX = 15;
constexpr int64_t kGlyphOffsetY = 24;
constexpr int64_t kPressedOffsetX = 3;
constexpr int64_t kPressedOffsetY = 12;

struct CloseLayer {
    int32_t source_x;
    int32_t source_y;
    int32_t width;
    int32_t height;
    int32_t offset_x;
    int32_t offset_y;
};

// loc3 金属框只取边框分片（相对 loc3 原点）。loc3 左侧开口、中央烘焙返回箭头，故不取左中与
// 中央，只拼上/下/右边与右角弧；条带与角弧刻意重叠，避免 1:1 采样在接缝处露缝。
struct FrameSlice {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};
constexpr FrameSlice kFrameSlices[] = {
    {0, 4, 78, 5},     // 上边（含左上/右上角上沿）
    {0, 76, 78, 5},    // 下边（含左下/右下角下沿）
    {68, 4, 6, 78},    // 右边（含右角外沿）
    {60, 4, 18, 18},   // 右上角弧
    {60, 64, 18, 22},  // 右下角弧
};

bool read_layer(void* location, CloseLayer* out) {
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
    constexpr int64_t min = std::numeric_limits<int32_t>::min();
    constexpr int64_t max = std::numeric_limits<int32_t>::max();
    if (rect.x < min || rect.x > max || rect.y < min || rect.y > max ||
        rect.w <= 0 || rect.h <= 0 || rect.w > max || rect.h > max) return false;
    return rect.x <= max - rect.w && rect.y <= max - rect.h;
}

bool point_fits_int32(int64_t x, int64_t y) {
    return x >= std::numeric_limits<int32_t>::min() &&
        x <= std::numeric_limits<int32_t>::max() &&
        y >= std::numeric_limits<int32_t>::min() &&
        y <= std::numeric_limits<int32_t>::max();
}

bool load_group(int32_t unit, void** out_group) {
    if (out_group == nullptr || fn_imgsys_unit_load == nullptr || fn_imgsys_get_group == nullptr) {
        return false;
    }
    // 共享图集由游戏拥有：只确保加载，任何路径都不卸载。
    fn_imgsys_unit_load(unit);
    *out_group = fn_imgsys_get_group(unit);
    return *out_group != nullptr && game_memory_accessible(*out_group, sizeof(void*), 'r');
}

void draw_part(void* group, int64_t x, int64_t y, int32_t source_x, int32_t source_y,
               int32_t width, int32_t height, int32_t offset_x, int32_t offset_y) {
    fn_sgl_draw_texture_part_ex(group, static_cast<int32_t>(x), static_cast<int32_t>(y), source_x,
                                source_y, width, height, offset_x, offset_y, kNativeDrawMode, 0,
                                kNativeScalePercent, 0);
}

}  // namespace

bool ui_draw_native_close_button(UiRect rect, uint8_t pressed) {
    if (pressed > 1 || !rect_fits_int32(rect) || !game_host_is_main_thread() ||
        fn_sgl_draw_texture_part_ex == nullptr || fn_imgsys_get_loc == nullptr) {
        return false;
    }
    void* glyph_group = nullptr;
    if (!load_group(kGlyphImageUnit, &glyph_group)) return false;
    CloseLayer normal{};
    if (!read_layer(fn_imgsys_get_loc(kGlyphImageUnit, kGlyphNormalLoc), &normal)) return false;
    CloseLayer pressed_layer{};
    if (pressed != 0 &&
        !read_layer(fn_imgsys_get_loc(kGlyphImageUnit, kGlyphPressedLoc), &pressed_layer)) {
        return false;
    }

    // 原生尺寸盒 78×86 居中于 rect：与返回按钮同盒、同框、同尺寸，仅字形与按下光效不同。
    const int64_t origin_x = rect.x + (rect.w - kFrameWidth) / 2;
    const int64_t origin_y = rect.y + (rect.h - kFrameHeight) / 2;
    if (!point_fits_int32(origin_x, origin_y) ||
        !point_fits_int32(origin_x + kFrameWidth, origin_y + kFrameHeight)) {
        return false;
    }

    // 金属框借用共享 0x59 loc3 的边框分片；图集/分片不可用时降级为仅字形，不静默改尺寸。
    void* frame_group = nullptr;
    CloseLayer frame{};
    if (load_group(kFrameImageUnit, &frame_group) &&
        read_layer(fn_imgsys_get_loc(kFrameImageUnit, kFrameLoc), &frame)) {
        for (const FrameSlice& slice : kFrameSlices) {
            draw_part(frame_group, origin_x + slice.x, origin_y + slice.y,
                      frame.source_x + slice.x, frame.source_y + slice.y, slice.w, slice.h,
                      frame.offset_x, frame.offset_y);
        }
    }

    // X 字形按原生 1:1（loc0x3a 38×39 @ +(15,24)）。
    draw_part(glyph_group, origin_x + kGlyphOffsetX, origin_y + kGlyphOffsetY, normal.source_x,
              normal.source_y, normal.width, normal.height, normal.offset_x, normal.offset_y);
    if (pressed == 0) return true;
    // 按下叠原生橙色底板 1:1（loc0x34 62×61 @ +(3,12)）。
    draw_part(glyph_group, origin_x + kPressedOffsetX, origin_y + kPressedOffsetY,
              pressed_layer.source_x, pressed_layer.source_y, pressed_layer.width,
              pressed_layer.height, pressed_layer.offset_x, pressed_layer.offset_y);
    return true;
}

}  // namespace qol::game
