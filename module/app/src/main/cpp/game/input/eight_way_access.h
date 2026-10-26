#pragma once

#include <cstdint>

// 八向移动低层游戏访问：KEY 键码/键表、方向触摸按住位、屏幕高度、TouchHandle 偏移。
// 只处理地址/ABI，不持有八向状态；所有函数在游戏主线程调用。

namespace qol::game::input::access {

bool key_code(int32_t* out);
bool write_key_code(int32_t code);

// 复制原版方向键码表：out[0]=左、out[1]=右、out[2]=上、out[3]=下。
bool read_key_table(int32_t out[4]);

bool press_active();
bool write_press_active(bool active);
int32_t screen_height();

// TouchHandle_GetControlOffset 前两个 int64 字段（屏幕坐标偏移）。
bool touch_offset(int64_t* x, int64_t* y);

// 控件绝对左上角（ControlObject_GetAbsolutePoint，递归父链）。
bool control_absolute_point(void* ctrl, int64_t* x, int64_t* y);

}  // namespace qol::game::input::access
