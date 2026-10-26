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

// 自定义触摸移动前把朝向预置为主方向，并清掉原版 8px 转向等待位。
// 原版 GAMESTATE_PressKeyPlay 只有在朝向相同或坐标 8px 对齐时才允许转向；
// 角色可能因地图切换或其他输入处于非 8px 坐标，若不预置朝向，原版会进入
// CHAR_MoveOnWaitCoord 补步，表现为方向切换后移动变慢、松手后仍继续移动。
bool prepare_touch_move(void* ch, int32_t dir);

// 清除原版转向等待位（仅由本功能拥有的触摸输入调用）。
bool clear_wait_coord(void* ch);

// 虚拟按键根下按 ID 查找控件。
void* virtual_control(int32_t id);

// 虚拟按键根下的方向摇杆控件（ID 0x15）。
void* virtual_pad_control();

}  // namespace qol::game::input::access
