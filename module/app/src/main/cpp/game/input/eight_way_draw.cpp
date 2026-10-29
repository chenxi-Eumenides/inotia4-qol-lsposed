#include "game/input/eight_way_draw.h"

#include <atomic>
#include <cmath>
#include <cstdint>

#include "game/access/game_access.h"
#include "game/access/qol_log_seam.h"
#include "game/access/world.h"
#include "game/input/eight_way_access.h"
#include "game/input/eight_way_state.h"
#include "game/symbols/game_symbols.h"

namespace qol::game::input {
namespace {

using GetGroupFn = void* (*)(int32_t);
using GetLocFn = void* (*)(int32_t, int32_t);
using DrawTexturePartExFn = void (*)(void*, int32_t, int32_t, int32_t, int32_t, int32_t,
                                     int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                                     int32_t);
using DrawLineFn = void (*)(int32_t, int32_t, int32_t, int32_t, uint32_t);
using FillRectFn = void (*)(int32_t, int32_t, int32_t, int32_t, uint32_t);

constexpr int32_t kPadHalfSize = 126;
constexpr float kOrbitRadius = 92.0f;
constexpr float kRadius = 96.0f;
constexpr float kTail = 12.0f;
constexpr float kHeadLength = 16.0f;
constexpr float kHeadHalf = 9.0f;
constexpr uint32_t kIndicatorColor = 0xFF00D7FFu;
// 原版箭头 loc：上箭头 0x0f；其贴图基准朝上。
constexpr int32_t kUpArrowLoc = 0x0f;
constexpr int32_t kArrowControlFirst = 0x1e;
constexpr int32_t kArrowControlCount = 4;
constexpr int64_t kArrowOffscreen = -10000;
constexpr double kRadToDeg = 57.29577951308232;

struct SavedRect {
    int64_t x = 0;
    int64_t y = 0;
    int64_t w = 0;
    int64_t h = 0;
    bool valid = false;
};

SavedRect g_saved_arrow_rect[kArrowControlCount];
bool g_overlay_hidden = false;
uint32_t g_overlay_tick = 0;
void* g_overlay_controls[kArrowControlCount]{};

GetGroupFn get_group_fn() {
    static GetGroupFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<GetGroupFn>(
            g_base + fn_resolve("F_IMGSYS_GET_GROUP_VMA", F_IMGSYS_GET_GROUP_VMA));
    }
    return fn;
}

GetLocFn get_loc_fn() {
    static GetLocFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<GetLocFn>(
            g_base + fn_resolve("F_IMGSYS_GET_LOC_VMA", F_IMGSYS_GET_LOC_VMA));
    }
    return fn;
}

DrawTexturePartExFn draw_texture_part_ex_fn() {
    static DrawTexturePartExFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<DrawTexturePartExFn>(
            g_base + fn_resolve("F_SGL_DRAW_TEXTURE_PART_EX_VMA",
                                F_SGL_DRAW_TEXTURE_PART_EX_VMA));
    }
    return fn;
}

DrawLineFn draw_line_fn() {
    static DrawLineFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<DrawLineFn>(
            g_base + fn_resolve("F_GRPX_DRAW_LINE_VMA", F_GRPX_DRAW_LINE_VMA));
    }
    return fn;
}

FillRectFn fill_rect_fn() {
    static FillRectFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<FillRectFn>(
            g_base + fn_resolve("F_GRPX_FILL_RECT_VMA", F_GRPX_FILL_RECT_VMA));
    }
    return fn;
}

void draw_fallback_line(float cx, float cy, float ux, float uy) {
    const int32_t tail_x = static_cast<int32_t>(std::lround(cx + ux * kTail));
    const int32_t tail_y = static_cast<int32_t>(std::lround(cy - uy * kTail));
    const int32_t tip_x = static_cast<int32_t>(std::lround(cx + ux * kRadius));
    const int32_t tip_y = static_cast<int32_t>(std::lround(cy - uy * kRadius));
    const int32_t wing1_x = static_cast<int32_t>(
        std::lround(tip_x - ux * kHeadLength + uy * kHeadHalf));
    const int32_t wing1_y = static_cast<int32_t>(
        std::lround(tip_y + uy * kHeadLength + ux * kHeadHalf));
    const int32_t wing2_x = static_cast<int32_t>(
        std::lround(tip_x - ux * kHeadLength - uy * kHeadHalf));
    const int32_t wing2_y = static_cast<int32_t>(
        std::lround(tip_y + uy * kHeadLength - ux * kHeadHalf));

    const DrawLineFn draw_line = draw_line_fn();
    if (draw_line == nullptr) return;
    draw_line(tail_x, tail_y, tip_x, tip_y, kIndicatorColor);
    draw_line(tip_x, tip_y, wing1_x, wing1_y, kIndicatorColor);
    draw_line(tip_x, tip_y, wing2_x, wing2_y, kIndicatorColor);
    const FillRectFn fill_rect = fill_rect_fn();
    if (fill_rect != nullptr) {
        fill_rect(static_cast<int32_t>(cx) - 2, static_cast<int32_t>(cy) - 2, 4, 4,
                  kIndicatorColor);
    }
}

}  // namespace

void prepare_direction_overlay() {
    if (!enabled()) return;
    // 箭头控件在启用时移出屏幕一次即可；游戏重建控件或每隔 30 帧复核一次。
    // 原实现每帧对 4 个控件做 control_rect + set_control_rect，占 world_idle
    // 约 4% CPU，且大部分帧没有任何位置变化。
    bool controls_changed = false;
    for (int32_t i = 0; i < kArrowControlCount; ++i) {
        void* ctrl = access::virtual_control(kArrowControlFirst + i);
        if (ctrl != g_overlay_controls[i]) {
            g_overlay_controls[i] = ctrl;
            controls_changed = true;
        }
    }
    if (g_overlay_hidden && !controls_changed && (++g_overlay_tick % 30) != 0) return;
    for (int32_t i = 0; i < kArrowControlCount; ++i) {
        void* ctrl = g_overlay_controls[i];
        if (ctrl == nullptr) continue;
        int64_t rect[4]{};
        if (!access::control_rect(ctrl, rect)) continue;
        SavedRect& saved = g_saved_arrow_rect[i];
        if (!saved.valid) {
            saved.x = rect[0];
            saved.y = rect[1];
            saved.w = rect[2];
            saved.h = rect[3];
            saved.valid = true;
        }
        if (rect[0] != kArrowOffscreen || rect[1] != kArrowOffscreen) {
            (void)access::set_control_rect(ctrl, kArrowOffscreen, kArrowOffscreen, rect[2],
                                           rect[3]);
        }
    }
    g_overlay_hidden = true;
}

void restore_direction_overlay() {
    g_overlay_hidden = false;
    g_overlay_tick = 0;
    for (int32_t i = 0; i < kArrowControlCount; ++i) {
        SavedRect& saved = g_saved_arrow_rect[i];
        if (!saved.valid) continue;
        void* ctrl = access::virtual_control(kArrowControlFirst + i);
        if (ctrl != nullptr) {
            (void)access::set_control_rect(ctrl, saved.x, saved.y, saved.w, saved.h);
        }
        g_overlay_controls[i] = nullptr;
    }
}

void draw_direction_indicator() {
    if (!enabled()) return;
    const State& state = current_state();
    if (!state.active || (state.selected_dx == 0 && state.selected_dy == 0)) return;
    if (world_game_state_gate() != 0) return;

    const float dx = static_cast<float>(state.selected_dx);
    const float dy = static_cast<float>(state.selected_dy);
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f) return;

    // 选中落点向量用右正、上正；屏幕坐标的 y 轴向下。
    const float ux = dx / length;
    const float uy = dy / length;

    void* pad = access::virtual_pad_control();
    if (pad == nullptr) return;
    int64_t left = 0;
    int64_t top = 0;
    if (!access::control_absolute_point(pad, &left, &top)) return;
    const float cx = static_cast<float>(left + kPadHalfSize);
    const float cy = static_cast<float>(top + kPadHalfSize);

    const GetGroupFn get_group = get_group_fn();
    const GetLocFn get_loc = get_loc_fn();
    const DrawTexturePartExFn draw_part_ex = draw_texture_part_ex_fn();
    if (get_group == nullptr || get_loc == nullptr || draw_part_ex == nullptr) {
        draw_fallback_line(cx, cy, ux, uy);
        return;
    }

    void* group = get_group(0x16);
    void* loc = get_loc(0x16, kUpArrowLoc);
    if (group == nullptr || loc == nullptr) {
        draw_fallback_line(cx, cy, ux, uy);
        return;
    }

    const auto* rect = static_cast<const int32_t*>(loc);
    const int32_t sx = rect[0];
    const int32_t sy = rect[1];
    const int32_t sw = rect[2];
    const int32_t sh = rect[3];
    const int32_t offset_x = rect[4];
    const int32_t offset_y = rect[5];

    // 上箭头基准朝上：选中落点向量的屏幕角度（0=右，-90=上）+90 得到旋转角。
    const double screen_angle_deg = std::atan2(-static_cast<double>(dy),
                                               static_cast<double>(dx)) * kRadToDeg;
    int32_t rotation = static_cast<int32_t>(std::lround(screen_angle_deg + 90.0));
    rotation %= 360;
    if (rotation < 0) rotation += 360;

    // SGL 的 x/y 是未旋转贴图左上角；绕中心旋转需要把旋转后的左上角算回来。
    const double theta = static_cast<double>(rotation) / kRadToDeg;
    const double cos_t = std::cos(theta);
    const double sin_t = std::sin(theta);
    const double half_w = sw * 0.5;
    const double half_h = sh * 0.5;
    const int32_t draw_x = static_cast<int32_t>(std::lround(
        cx + ux * kOrbitRadius - half_w * cos_t + half_h * sin_t));
    const int32_t draw_y = static_cast<int32_t>(std::lround(
        cy - uy * kOrbitRadius - half_w * sin_t - half_h * cos_t));

    draw_part_ex(group, draw_x, draw_y, sx, sy, sw, sh, offset_x, offset_y, 2, 0, 100,
                 rotation);

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        QOL_LOG_SEAM_INFO(qol::game::kQolLogDomainPlatform,
                          "movement sprite selected=(%d,%d) center=(%d,%d) rot=%d wh=%dx%d draw=(%d,%d)",
                          state.selected_dx, state.selected_dy, static_cast<int32_t>(cx),
                          static_cast<int32_t>(cy), rotation, sw, sh, draw_x, draw_y);
    }
}

}  // namespace qol::game::input
