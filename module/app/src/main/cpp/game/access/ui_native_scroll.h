#pragma once

#include <algorithm>
#include <cstring>
#include <limits>

#include "game/access/game_access.h"
#include "game/access/ui_kit.h"
#include "game/adapter_ports/game_host_port.h"

namespace qol::game {

struct NativeScrollState {
    int64_t offset;
    int64_t max_offset;
    int64_t key_step;
};

// 不把 ControlScroll data 布局或负位移传播到消费者。
inline bool native_scroll_state(void* scroll, NativeScrollState* out) {
    if (scroll == nullptr || out == nullptr || fn_ctrl_get_data == nullptr ||
        !game_memory_accessible(scroll, CO_SIZE, 'r')) return false;
    const auto* data = static_cast<const uint8_t*>(fn_ctrl_get_data(scroll));
    if (data == nullptr || !game_memory_accessible(data, CS_SIZE, 'r') ||
        data[CS_TYPE] != 1) return false;
    int64_t start, current, extent, view, step;
    std::memcpy(&start, data + CS_START, sizeof(start));
    std::memcpy(&current, data + CS_CURRENT, sizeof(current));
    std::memcpy(&extent, data + CS_CONTENT_EXTENT, sizeof(extent));
    std::memcpy(&view, data + CS_VIEW_EXTENT, sizeof(view));
    std::memcpy(&step, data + CS_KEY_STEP, sizeof(step));
    if (extent < 0 || view <= 0 || start < current) return false;
    const uint64_t offset = static_cast<uint64_t>(start) - static_cast<uint64_t>(current);
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    *out = NativeScrollState{static_cast<int64_t>(offset), extent > view ? extent - view : 0, step};
    return true;
}

// ---- 我方自建 vertical/type0 scroll 的局部 draw plumbing ----
//
// 复现原生 ControlScroll_Draw 的绘制顺序（内容回调 → bar），但把 bar 约束在 parent∩screen：
// 原生整段 Draw 会在末尾 reset clip 后画 bar，父裁切下会泄漏，故本路径不调用它（失败也不回退）。
// 只读受控字段、不写 control 内存、不改 Process/drag/配色/几何、不缓存跨帧、不新增 hook。
// 任何 preflight 失败：不写 clip、不绘制、返回 false；空 content 交集跳过内容与 bar 并返回 true。
inline bool native_scroll_draw_clipped(void* scroll) {
    if (scroll == nullptr || g_base == 0) return false;
    if (!game_host_is_main_thread()) return false;
    if (fn_ctrl_get_data == nullptr || fn_grpx_set_clip == nullptr ||
        fn_grpx_reset_clip == nullptr || fn_ui_draw_scrollbar == nullptr ||
        fn_calc_res_width == nullptr || fn_calc_res_height == nullptr) {
        return false;
    }
    if (!game_memory_accessible(scroll, CO_SIZE, 'r')) return false;
    const uint8_t* data = static_cast<const uint8_t*>(fn_ctrl_get_data(scroll));
    if (data == nullptr || !game_memory_accessible(data, CS_SIZE, 'r')) return false;
    if (data[CS_TYPE] != 1) return false;  // 只覆盖我方自建 vertical scroll

    const auto add_ok = [](int64_t a, int64_t b, int64_t* out) {
        if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
            (b < 0 && a < std::numeric_limits<int64_t>::min() - b)) {
            return false;
        }
        *out = a + b;
        return true;
    };
    const auto sub_ok = [](int64_t a, int64_t b, int64_t* out) {
        if ((b > 0 && a < std::numeric_limits<int64_t>::min() + b) ||
            (b < 0 && a > std::numeric_limits<int64_t>::max() + b)) {
            return false;
        }
        *out = a - b;
        return true;
    };
    const auto fits_i32 = [](int64_t v) {
        return v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max();
    };

    int64_t start = 0, current = 0, content = 0, view = 0;
    int64_t bar_x = 0, bar_y = 0, bar_len = 0, bar_arg = 0;
    // bar 可见性与 bar type 在原生都只读 1 字节（`ldrb w0,[x19,#0xa8]` / `ldrb w6,[x19,#0xc8]`），
    // 按同宽读取：padding 或高位不参与判断，type 只要求该字节为 0。
    uint8_t bar_visible = 0;
    uint8_t bar_type = 0;
    void (*content_cb)(void*, int64_t) = nullptr;
    std::memcpy(&start, data + CS_START, sizeof(start));
    std::memcpy(&current, data + CS_CURRENT, sizeof(current));
    std::memcpy(&content, data + CS_CONTENT_EXTENT, sizeof(content));
    std::memcpy(&view, data + CS_VIEW_EXTENT, sizeof(view));
    std::memcpy(&bar_visible, data + CS_BAR_VISIBLE, sizeof(bar_visible));
    std::memcpy(&bar_x, data + CS_BAR_X, sizeof(bar_x));
    std::memcpy(&bar_y, data + CS_BAR_Y, sizeof(bar_y));
    std::memcpy(&bar_len, data + CS_BAR_LENGTH, sizeof(bar_len));
    std::memcpy(&bar_type, data + CS_BAR_DRAW_TYPE, sizeof(bar_type));
    std::memcpy(&bar_arg, data + CS_BAR_ARG, sizeof(bar_arg));
    std::memcpy(&content_cb, data + CS_CALLBACK, sizeof(content_cb));
    if (content < 0 || view <= 0) return false;

    // 自身 rect 与有界父链绝对原点：逐节点先验可读再取自身 rect 并 checked 累加。
    // 旧 ui_control_absolute_origin 对未验证链无界 raw 读取（可读/成环/溢出均无保护），
    // 不能直接复用；此处用受控 helper（ui_get_rect / ui_control_parent）实现同样语义。
    UiRect self_rect{};
    int64_t abs_x = 0;
    int64_t abs_y = 0;
    {
        int depth = 0;
        for (void* node = scroll; node != nullptr; node = ui_control_parent(node)) {
            if (++depth > 64) return false;  // 链异常/成环：不信任
            if (!game_memory_accessible(node, CO_SIZE, 'r')) return false;
            UiRect node_rect{};
            if (!ui_get_rect(node, &node_rect)) return false;
            if (depth == 1) self_rect = node_rect;
            if (!add_ok(abs_x, node_rect.x, &abs_x) || !add_ok(abs_y, node_rect.y, &abs_y)) {
                return false;
            }
        }
    }
    if (self_rect.w <= 0 || self_rect.h <= 0) return false;

    UiRect parent{};
    if (!ui_clip_snapshot(&parent)) return false;
    // 四值先一律要求 int32 可表示：哨兵也不接受超 int32 的垃圾值（回写/求交都在 int32 域内）。
    if (!fits_i32(parent.x) || !fits_i32(parent.y) || !fits_i32(parent.w) ||
        !fits_i32(parent.h)) {
        return false;
    }
    const bool has_parent = parent.x >= 0;
    // 尺寸为正只对真实父 clip 要求；哨兵（-1）原样保留并走 reset 恢复。
    if (has_parent && (parent.w <= 0 || parent.h <= 0)) return false;

    const int32_t screen_w = fn_calc_res_width();
    const int32_t screen_h = fn_calc_res_height();
    if (screen_w <= 0 || screen_h <= 0) return false;

    // |current - start|：先 checked 相减，再按无符号取幅值（INT64_MIN 也安全），
    // 避免 signed overflow 与 -INT64_MIN 的 UB。
    int64_t delta = 0;
    if (!sub_ok(current, start, &delta)) return false;
    const uint64_t magnitude =
        delta >= 0 ? static_cast<uint64_t>(delta) : ~static_cast<uint64_t>(delta) + 1u;
    if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    const int64_t offset = static_cast<int64_t>(magnitude);

    struct Rect {
        int64_t x, y, w, h;
    };
    const auto intersect = [&](const Rect& a, const Rect& b, Rect* out) {
        int64_t ax2 = 0, ay2 = 0, bx2 = 0, by2 = 0;
        if (!add_ok(a.x, a.w, &ax2) || !add_ok(a.y, a.h, &ay2) || !add_ok(b.x, b.w, &bx2) ||
            !add_ok(b.y, b.h, &by2)) {
            return false;
        }
        const int64_t x = std::max(a.x, b.x);
        const int64_t y = std::max(a.y, b.y);
        const int64_t x2 = std::min(ax2, bx2);
        const int64_t y2 = std::min(ay2, by2);
        if (x2 <= x || y2 <= y) {
            *out = Rect{x, y, 0, 0};  // 空交集
            return true;
        }
        *out = Rect{x, y, x2 - x, y2 - y};
        return true;
    };

    const Rect screen{0, 0, screen_w, screen_h};
    const Rect viewport{abs_x, abs_y, self_rect.w, self_rect.h};
    Rect content_clip{};
    if (!intersect(viewport, screen, &content_clip)) return false;
    if (has_parent) {
        const Rect parent_rect{parent.x, parent.y, parent.w, parent.h};
        if (!intersect(content_clip, parent_rect, &content_clip)) return false;
    }
    if (content_clip.w <= 0 || content_clip.h <= 0) return true;  // 空交集：内容与 bar 都不画

    // bar：parent∩screen（不是 viewport；原生 bar 位于 viewport 右 6px）。有 bar 必须严格 type0。
    Rect bar_clip{0, 0, 0, 0};
    bool bar_ready = false;
    if (bar_visible != 0) {
        if (bar_type != 0) return false;
        if (bar_len < 0) return false;
        bar_clip = screen;
        if (has_parent) {
            const Rect parent_rect{parent.x, parent.y, parent.w, parent.h};
            if (!intersect(bar_clip, parent_rect, &bar_clip)) return false;
        }
        bar_ready = bar_clip.w > 0 && bar_clip.h > 0;
    }
    int64_t bar_abs_x = 0;
    int64_t bar_abs_y = 0;
    if (bar_ready) {
        if (!add_ok(abs_x, bar_x, &bar_abs_x) || !add_ok(abs_y, bar_y, &bar_abs_y)) return false;
        if (!fits_i32(bar_abs_x) || !fits_i32(bar_abs_y) || !fits_i32(bar_len) || !fits_i32(offset) ||
            !fits_i32(content) || !fits_i32(view)) {
            return false;
        }
    }
    // 写 clip 前的最后一道边界：两段矩形四值都必须 int32 可表示（下方 static_cast 不做检查）。
    const auto rect_fits_i32 = [&](const Rect& r) {
        return fits_i32(r.x) && fits_i32(r.y) && fits_i32(r.w) && fits_i32(r.h);
    };
    if (!rect_fits_i32(content_clip) || !rect_fits_i32(bar_clip)) return false;

    // 入口真实 clip 的恢复守卫：构造/析构都不写 clip，只有真正 set 过一次后才在退出时恢复，
    // 因此 preflight 失败与空交集仍零写。析构走普通 scope 退出（含 callback unwind），
    // 非哨兵逐值写回快照，哨兵用 reset。
    struct ClipRestoreGuard {
        const UiRect* parent;
        bool has_parent;
        bool written;
        ~ClipRestoreGuard() {
            if (!written) return;
            if (has_parent) {
                fn_grpx_set_clip(static_cast<int32_t>(parent->x), static_cast<int32_t>(parent->y),
                                 static_cast<int32_t>(parent->w), static_cast<int32_t>(parent->h));
            } else {
                fn_grpx_reset_clip();
            }
        }
    };
    ClipRestoreGuard guard{&parent, has_parent, false};
    const auto apply_clip = [&](const Rect& r) {
        guard.written = true;  // 先置位：写入之后必经析构恢复
        fn_grpx_set_clip(static_cast<int32_t>(r.x), static_cast<int32_t>(r.y),
                         static_cast<int32_t>(r.w), static_cast<int32_t>(r.h));
    };
    // preflight 已全部通过：此后无提前返回，恢复由 guard 统一负责。
    apply_clip(content_clip);
    if (content_cb != nullptr) content_cb(scroll, current);
    if (bar_ready) {
        apply_clip(bar_clip);
        fn_ui_draw_scrollbar(bar_abs_x, bar_abs_y, bar_len, offset, content, view,
                            static_cast<int64_t>(bar_type), bar_arg);
    }
    return true;
}

}  // namespace qol::game
