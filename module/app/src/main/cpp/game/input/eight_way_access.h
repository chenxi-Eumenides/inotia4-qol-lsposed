#pragma once

#include <cstdint>

// 全向移动低层游戏访问：KEY 键码/键表、方向触摸按住位、屏幕高度、TouchHandle 偏移、
// 角色坐标/路径、碰撞查询。
// 只处理地址/ABI，不持有移动状态；所有函数在游戏主线程调用。

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

// 角色坐标（+0x2/+0x4）与路径列表（+0x2f0）。
bool read_position(const void* ch, int16_t* x, int16_t* y);
bool write_position(void* ch, int16_t x, int16_t y);
bool has_path(const void* ch);

// 自定义触摸移动前把朝向预置为主方向，并清掉原版 8px 转向等待位。
bool prepare_touch_move(void* ch, int32_t dir);

// 清除原版转向等待位（+0x2e0）。
bool clear_wait_coord(void* ch);

// 碰撞查询：对齐坐标透传原函数；非对齐坐标查询实际 16px 碰撞盒覆盖的瓦片。
// original 必须是 hook 提供的原函数 trampoline；直接调用函数入口会再次进入 hook。
using MapBlockingQueryFn = int32_t (*)(int32_t, int32_t);
int32_t map_blocking_query(int32_t x, int32_t y, MapBlockingQueryFn original);

// 瓦片级阻挡（MAP_IsBlocking），用于吸附点校验。
int32_t tile_blocking(int32_t tile_x, int32_t tile_y);

// 摄像机焦点（MAP_SetFocus，像素坐标）。
bool set_camera_focus(int32_t x, int32_t y);

// 虚拟按键根下按 ID 查找控件。
void* virtual_control(int32_t id);

// 虚拟按键根下的方向摇杆控件（ID 0x15）。
void* virtual_pad_control();

}  // namespace qol::game::input::access
