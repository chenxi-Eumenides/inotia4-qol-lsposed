#pragma once

namespace qol::game::input {

// UIPlayPorting_Draw 原函数返回后调用：斜向时在原版已点亮主轴箭头的基础上，
// 补画副轴箭头，表现“两个方向同时亮”。
void draw_secondary_arrow();

}  // namespace qol::game::input
