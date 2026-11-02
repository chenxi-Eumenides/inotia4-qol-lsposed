#include "game/input/eight_way_move.h"

#include <atomic>
#include <cstdint>

#include "game/access/game_access.h"
#include "game/access/qol_log_seam.h"
#include "game/access/world.h"
#include "game/input/eight_way_access.h"
#include "game/input/eight_way_move_math.h"
#include "game/input/eight_way_state.h"
#include "game/state/game_state.h"
#include "game/symbols/game_symbols.h"

namespace qol::game::input {
namespace {

void log_first_axis(int32_t delta, int32_t step) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) return;
    const State& state = current_state();
    QOL_LOG_SEAM_INFO(kQolLogDomainPlatform,
                      "movement move axis mode=%u step=%d dir=%d applied=%d delta=%d",
                      static_cast<unsigned>(state.mode), state.candidate_step,
                      state.primary_dir, step, delta);
}

void log_first_omni(int32_t delta, int32_t primary_step, int32_t secondary_step) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) return;
    const State& state = current_state();
    QOL_LOG_SEAM_INFO(
        kQolLogDomainPlatform,
        "movement move mode=%u step=%d lattice=%d primary=%d/%d secondary=%d/%d delta=%d",
        static_cast<unsigned>(state.mode), state.candidate_step, state.lattice_index,
        state.primary_dir, primary_step, state.secondary_dir, secondary_step, delta);
}

int32_t pass_through(void* ch, int32_t dir, int32_t delta, uint8_t flag) {
    return fn_char_move(ch, dir, delta, flag);
}

}  // namespace

int32_t char_move(void* ch, int32_t dir, int32_t delta, uint8_t flag) {
    if (fn_char_move == nullptr) return 0;

    const State& state = current_state();
    // 轴向没有 secondary_dir，但步长仍可能被 better_move.step 档位放大/缩小，
    // 不能在这里透传，否则十字方向永远保持原版 8px。
    if (!enabled() || !state.active || !state.trusted || flag != 0 || dir < 0 || dir > 3 ||
        dir != state.primary_dir ||
        (state.primary_step == 0 && state.secondary_step == 0)) {
        return pass_through(ch, dir, delta, flag);
    }

    void* lead = lead_member();
    if (lead == nullptr || ch != lead || world_game_state_gate() != 0 || access::has_path(ch)) {
        return pass_through(ch, dir, delta, flag);
    }

    const int32_t step = state.candidate_step > 0 ? state.candidate_step : current_step();
    const int32_t ceiling = scaled_step_ceiling(step, delta);
    const int32_t primary_step = scale_component(state.primary_step, delta, ceiling);
    const int32_t secondary_step = scale_component(state.secondary_step, delta, ceiling);
    if (primary_step <= 0 && secondary_step <= 0) {
        return pass_through(ch, dir, delta, flag);
    }
    if (secondary_step <= 0) {
        log_first_axis(delta, primary_step);
        return fn_char_move(ch, state.primary_dir, primary_step, flag);
    }
    if (primary_step <= 0) {
        return fn_char_move(ch, state.secondary_dir, secondary_step, flag);
    }

    int16_t origin_x = 0;
    int16_t origin_y = 0;
    if (!access::read_position(ch, &origin_x, &origin_y)) {
        return pass_through(ch, dir, delta, flag);
    }

    // 副轴先静默移动（flag=1），主轴完整移动（flag=0）负责摄像机/格子事件。
    // 主轴被挡时回滚副轴并按完整模式重走副轴，实现贴墙滑行。
    const int32_t secondary_result =
        fn_char_move(ch, state.secondary_dir, secondary_step, 1);
    if (secondary_result == 3) return 3;

    const int32_t primary_result = fn_char_move(ch, state.primary_dir, primary_step, 0);
    if (primary_result == 0) {
        log_first_omni(delta, primary_step, secondary_step);
        return 0;
    }
    if (primary_result == 3) {
        (void)access::write_position(ch, origin_x, origin_y);
        return 3;
    }

    if (secondary_result == 0) {
        constexpr int32_t kOpposite[4] = {2, 3, 0, 1};
        const int32_t rollback =
            fn_char_move(ch, kOpposite[state.secondary_dir], secondary_step, 1);
        if (rollback == 0) {
            const int32_t slide =
                fn_char_move(ch, state.secondary_dir, secondary_step, 0);
            if (slide == 0) {
                log_first_omni(delta, primary_step, secondary_step);
                return 0;
            }
        }
        // 回滚或滑行失败：恢复移动前坐标，避免留下静默位移。
        (void)access::write_position(ch, origin_x, origin_y);
        return 2;
    }

    return primary_result;
}

}  // namespace qol::game::input
