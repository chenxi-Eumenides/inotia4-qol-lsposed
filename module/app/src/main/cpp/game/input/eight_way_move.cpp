#include "game/input/eight_way_move.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "game/access/game_access.h"
#include "game/access/qol_log_seam.h"
#include "game/access/world.h"
#include "game/input/eight_way_move_math.h"
#include "game/input/eight_way_state.h"
#include "game/state/game_state.h"
#include "game/symbols/game_symbols.h"

namespace qol::game::input {
namespace {

void log_first_diagonal() {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) return;
    QOL_LOG_SEAM_INFO(kQolLogDomainPlatform, "eight_way diagonal applied");
}

bool has_path(const void* ch) {
    if (ch == nullptr) return false;
    const auto* bytes = static_cast<const uint8_t*>(ch);
    if (!game_memory_accessible(bytes + C_PATH_LIST, sizeof(void*), 'r')) return true;
    return *reinterpret_cast<void* const*>(bytes + C_PATH_LIST) != nullptr;
}

bool read_position(const void* ch, int16_t* x, int16_t* y) {
    if (ch == nullptr || x == nullptr || y == nullptr) return false;
    const auto* bytes = static_cast<const uint8_t*>(ch);
    if (!game_memory_accessible(bytes + C_POS_X, sizeof(int16_t), 'r') ||
        !game_memory_accessible(bytes + C_POS_Y, sizeof(int16_t), 'r')) {
        return false;
    }
    *x = *reinterpret_cast<const int16_t*>(bytes + C_POS_X);
    *y = *reinterpret_cast<const int16_t*>(bytes + C_POS_Y);
    return true;
}

bool restore_position(void* ch, int16_t x, int16_t y) {
    if (ch == nullptr) return false;
    auto* bytes = static_cast<uint8_t*>(ch);
    if (!game_memory_accessible(bytes + C_POS_X, sizeof(int16_t), 'w') ||
        !game_memory_accessible(bytes + C_POS_Y, sizeof(int16_t), 'w')) {
        return false;
    }
    *reinterpret_cast<int16_t*>(bytes + C_POS_X) = x;
    *reinterpret_cast<int16_t*>(bytes + C_POS_Y) = y;
    return true;
}

int32_t pass_through(void* ch, int32_t dir, int32_t delta, uint8_t flag) {
    return fn_char_move(ch, dir, delta, flag);
}

}  // namespace

int32_t char_move(void* ch, int32_t dir, int32_t delta, uint8_t flag) {
    if (fn_char_move == nullptr) return 0;

    const State& state = current_state();
    if (!should_apply_secondary(enabled(), state.active, state.trusted,
                                static_cast<int32_t>(flag), dir, state.primary_dir,
                                state.secondary_dir) ||
        delta <= 0 || dir < 0 || dir > 3 || state.secondary_dir == dir) {
        return pass_through(ch, dir, delta, flag);
    }

    void* lead = lead_member();
    if (lead == nullptr || ch != lead || world_game_state_gate() != 0 || has_path(ch)) {
        return pass_through(ch, dir, delta, flag);
    }

    int16_t origin_x = 0;
    int16_t origin_y = 0;
    if (!read_position(ch, &origin_x, &origin_y)) {
        return pass_through(ch, dir, delta, flag);
    }

    // 主轴先静默移动（flag=1）：避免经过侧格时触发侧格切图/事件。
    // 主轴失败立即停住，不产生“只走副轴”的最终位置；主轴成功后副轴完整移动
    // （flag=0），最终格由原版执行摄像机、水面和格子事件检查。
    const int32_t primary_result = fn_char_move(ch, dir, delta, 1);
    if (primary_result == 3) return 3;
    if (primary_result != 0) return primary_result;

    const int32_t secondary_result = fn_char_move(ch, state.secondary_dir, delta, 0);
    if (secondary_result == 0) {
        log_first_diagonal();
        return 0;
    }

    // 副轴被挡：主轴已经静默移位。回滚主轴后按完整模式重走主轴，保证镜头/格子事件
    // 在最终的单轴位置上执行，避免留下“只走主轴但未做完整检查”的位置。
    constexpr int32_t kOpposite[4] = {2, 3, 0, 1};
    const int32_t rollback = fn_char_move(ch, kOpposite[dir], delta, 1);
    if (rollback == 0) {
        log_first_diagonal();
        return fn_char_move(ch, dir, delta, 0);
    }

    // 回滚失败（极少见：移动期间动态单位占位）：直接恢复移动前坐标，避免留下
    // “静默位移但未做镜头/格子事件检查”的状态。返回 2 让原版跳过推动。
    (void)restore_position(ch, origin_x, origin_y);
    log_first_diagonal();
    return 2;
}

}  // namespace qol::game::input
