#pragma once

#include <cstdint>

// 八向移动状态机：触摸扇区 -> 自定义键码 -> 原版主方向键码。
// 只在游戏主线程调用；char_move 读取 current_state() 决定是否追加副轴。

namespace qol::game::input {

struct State {
    bool active = false;
    bool trusted = false;
    int32_t sector = -1;
    int32_t primary_dir = -1;
    int32_t secondary_dir = -1;
    int32_t primary_key = 0;
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

}  // namespace qol::game::input
