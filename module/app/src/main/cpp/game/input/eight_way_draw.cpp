#include "game/input/eight_way_draw.h"

#include <atomic>
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
using DrawPartFn = void (*)(void*, int32_t, int32_t, void*, int32_t, int32_t, int32_t);

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

DrawPartFn draw_part_fn() {
    static DrawPartFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<DrawPartFn>(
            g_base + fn_resolve("F_GRPX_DRAW_PART_VMA", F_GRPX_DRAW_PART_VMA));
    }
    return fn;
}

// 点亮箭头在 IMGSYS group 0x16 中的 loc。真机 KEY 表为 [-3,-4,-1,-2]，
// 绘制栈按 child 0..3 使用 [-4,-2,-3,-1]，即 child 顺序为 右/下/左/上；
// 对应点亮 loc：下=0x10、左=0x0d、上=0x0f、右=0x0e。
constexpr int32_t kLitArrowLoc[4] = {0x10, 0x0d, 0x0f, 0x0e};
constexpr int32_t kPadHalfSize = 126;

}  // namespace

void draw_secondary_arrow() {
    if (!enabled()) return;
    const State& state = current_state();
    if (!state.active || state.secondary_dir < 0 || state.secondary_dir > 3) return;
    if (world_game_state_gate() != 0) return;

    void* ctrl = access::virtual_pad_control();
    if (ctrl == nullptr) return;
    int64_t x = 0;
    int64_t y = 0;
    if (!access::control_absolute_point(ctrl, &x, &y)) return;
    x += kPadHalfSize;
    y += kPadHalfSize;

    const GetGroupFn get_group = get_group_fn();
    const GetLocFn get_loc = get_loc_fn();
    const DrawPartFn draw_part = draw_part_fn();
    if (get_group == nullptr || get_loc == nullptr || draw_part == nullptr) return;

    void* group = get_group(0x16);
    void* loc = get_loc(0x16, kLitArrowLoc[state.secondary_dir]);
    if (group == nullptr || loc == nullptr) return;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        QOL_LOG_SEAM_INFO(qol::game::kQolLogDomainPlatform,
                          "eight_way draw secondary=%d loc=0x%x at=(%lld,%lld)",
                          state.secondary_dir, kLitArrowLoc[state.secondary_dir],
                          static_cast<long long>(x), static_cast<long long>(y));
    }
    draw_part(group, static_cast<int32_t>(x), static_cast<int32_t>(y), loc, 2, 2, 0);
}

}  // namespace qol::game::input
