#pragma once

namespace qol::game::input {

// UIPlayPorting_Draw 原函数返回后调用：按原始触摸角度绘制旋转指针；
// 释放/死区后 state.raw_active=false，不绘制。
void draw_direction_indicator();

// UIPlayPorting_Draw 原函数执行前调用：隐藏原版四根固定箭头。
void prepare_direction_overlay();

// 插件关闭/停用时恢复原版四根固定箭头。
void restore_direction_overlay();

}  // namespace qol::game::input
