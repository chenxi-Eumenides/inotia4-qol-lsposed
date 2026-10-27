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

using DrawLineFn = void (*)(int32_t, int32_t, int32_t, int32_t, uint32_t);
using FillRectFn = void (*)(int32_t, int32_t, int32_t, int32_t, uint32_t);

constexpr int32_t kPadHalfSize = 126;
constexpr float kRadius = 96.0f;
constexpr float kTail = 12.0f;
constexpr float kHeadLength = 16.0f;
constexpr float kHeadHalf = 9.0f;
// ABGR8888：金色指针，和原版 HUD 的暗色底对比明显。
constexpr uint32_t kIndicatorColor = 0xFF00D7FFu;

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

}  // namespace

void draw_direction_indicator() {
    if (!enabled()) return;
    const State& state = current_state();
    if (!state.active || !state.raw_active) return;
    if (world_game_state_gate() != 0) return;

    const float dx = static_cast<float>(state.raw_dx);
    const float dy = static_cast<float>(state.raw_dy);
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f) return;

    // 原始触摸向量：dx 右为正，dy 上为正。屏幕坐标 y 向下，所以绘制时取 -dy。
    const float ux = dx / length;
    const float uy = dy / length;

    void* pad = access::virtual_pad_control();
    if (pad == nullptr) return;
    int64_t left = 0;
    int64_t top = 0;
    if (!access::control_absolute_point(pad, &left, &top)) return;
    const float cx = static_cast<float>(left + kPadHalfSize);
    const float cy = static_cast<float>(top + kPadHalfSize);

    const int32_t tail_x = static_cast<int32_t>(std::lround(cx + ux * kTail));
    const int32_t tail_y = static_cast<int32_t>(std::lround(cy - uy * kTail));
    const int32_t tip_x = static_cast<int32_t>(std::lround(cx + ux * kRadius));
    const int32_t tip_y = static_cast<int32_t>(std::lround(cy - uy * kRadius));

    // 屏幕方向向量 s=(ux,-uy)，垂直向量 p=(uy,ux)。
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

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        QOL_LOG_SEAM_INFO(qol::game::kQolLogDomainPlatform,
                          "omni indicator dx=%d dy=%d center=(%d,%d) tip=(%d,%d)",
                          state.raw_dx, state.raw_dy, static_cast<int32_t>(cx),
                          static_cast<int32_t>(cy), tip_x, tip_y);
    }
}

}  // namespace qol::game::input
