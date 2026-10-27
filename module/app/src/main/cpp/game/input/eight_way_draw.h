#pragma once

namespace qol::game::input {

// UIPlayPorting_Draw 原函数返回后调用：按原始触摸角度绘制旋转指针；
// 释放/死区后 state.raw_active=false，不绘制。
void draw_direction_indicator();

}  // namespace qol::game::input
