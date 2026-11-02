#pragma once

#include <cstdint>

#include "contracts/game_ports/movement_input_port.h"

// 触屏移动状态机：触摸向量 -> 模式候选格点 -> 自定义键码 -> 原版主方向键码。
// 只在游戏主线程调用；char_move 读取 current_state() 决定主/副轴移动。

namespace qol::game::input {

struct State {
    bool active = false;
    bool trusted = false;
    contracts::QolMovementInputMode mode = contracts::QolMovementInputMode::kDisabled;
    int32_t lattice_index = -1;
    int32_t primary_dir = -1;
    int32_t secondary_dir = -1;
    int32_t primary_step = 0;
    int32_t secondary_step = 0;
    // Selected integer grid vector; this is not delta-scaled or collision-adjusted movement.
    int32_t selected_dx = 0;
    int32_t selected_dy = 0;
    // 候选点缓存角度（atan2(dy,dx)，[0, 2*pi)）；指针直接使用，避免每帧反算。
    double selected_angle = 0.0;
    // 本次选择使用的基础步长；步长变化后旧的 lattice_index 不能用于滞回比较。
    int32_t candidate_step = 0;
    int32_t primary_key = 0;
    bool owns_input = false;  // 当前 KEY_nCode/按住位是否由本功能写入

    void set_selected_candidate(contracts::QolMovementInputMode selected_mode,
                                int32_t candidate_index, int32_t dx, int32_t dy,
                                double angle, int32_t step) noexcept {
        mode = selected_mode;
        lattice_index = candidate_index;
        selected_dx = dx;
        selected_dy = dy;
        selected_angle = angle;
        candidate_step = step;
    }

    void clear() noexcept { *this = State{}; }
    void reset_for_mode(contracts::QolMovementInputMode next_mode) noexcept {
        clear();
        mode = next_mode;
    }
};

bool enabled();
contracts::QolMovementInputMode current_mode();
const State& current_state();
void set_mode(contracts::QolMovementInputMode mode);
// 基础步长（像素），由 better_move 从 better_move.step 档位推入；<=0 回退原生 8。
void set_step(int32_t step);
int32_t current_step();
// Compatibility entry point: enabled=true still means the legacy omnidirectional mode.
void set_enabled(uint8_t enabled);
void reset();
void reset_state();
void poll();

void on_direction_touch(void* ctrl, uint64_t event, const void* point, uint64_t param3);
int32_t translate_press_key(int32_t key);

// 每个逻辑帧检查主控切换；旧主控立即吸附到当前 16px 瓦片中心。
void watch_lead_change();

// CHAR_MoveOnWaitCoord 入口：主控且无路径时清位并返回 1（由端口跳过原函数）。
uint8_t handle_wait_coord(void* ch);

}  // namespace qol::game::input
