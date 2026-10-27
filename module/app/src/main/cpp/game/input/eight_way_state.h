#pragma once

#include <cstdint>

// 全向移动状态机：触摸向量 -> 8px 格点 -> 自定义键码 -> 原版主方向键码。
// 只在游戏主线程调用；char_move 读取 current_state() 决定主/副轴移动。

namespace qol::game::input {

struct State {
    bool active = false;
    bool trusted = false;
    int32_t lattice_index = -1;
    int32_t primary_dir = -1;
    int32_t secondary_dir = -1;
    int32_t primary_step = 0;
    int32_t secondary_step = 0;
    int32_t primary_key = 0;
    bool raw_active = false;  // 原始触摸向量有效（用于旋转指针）
    int32_t raw_dx = 0;       // 右为正
    int32_t raw_dy = 0;       // 上为正
    bool owns_input = false;  // 当前 KEY_nCode/按住位是否由本功能写入
};

bool enabled();
const State& current_state();
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
