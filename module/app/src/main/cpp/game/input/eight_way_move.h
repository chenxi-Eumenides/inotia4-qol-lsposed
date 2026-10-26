#pragma once

#include <cstdint>

// 八向移动游戏层：触屏摇杆状态、自定义方向键码翻译、CHAR_Move 双轴扩展。
// 只在游戏主线程调用；插件经 QOL_PORT_MOVEMENT_INPUT 转发。

namespace qol::game::input {

// UIPlay_ButtonDirection 原函数执行后调用（event=1/0x40/2/4/8）。
void on_direction_touch(void* ctrl, uint64_t event, const void* point, uint64_t param3);

// GAMESTATE_PressKeyPlay 入口翻译：自定义方向键码 -> 原版主方向键码，并写回 KEY_nCode。
// 非自定义键码原样返回。
int32_t translate_press_key(int32_t key);

// GAMESTATE_PressKeyPlay+0x21c 的 bl CHAR_Move 替换体。
int32_t char_move(void* ch, int32_t dir, int32_t delta, uint8_t flag);

void set_enabled(uint8_t enabled);
void reset();
void poll();

}  // namespace qol::game::input
