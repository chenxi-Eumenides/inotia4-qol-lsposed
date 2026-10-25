#pragma once

#include <cstdint>

#include "../../contracts/ui.h"

namespace qol::features::core_ui {

using UiRect = contracts::UiRect;

// 形状绘制只做扫描线填充，不持有全局状态：adapter 由调用方在 UI 主线程传入。
// 与 ui_components 的槽位签名一致，由组件侧同名函数薄委托。
QolResult ui_shape_draw_rect(const contracts::QolUiAdapter* adapter,
                             const contracts::UiShapeDesc* desc);
QolResult ui_shape_draw_rounded_rect(const contracts::QolUiAdapter* adapter,
                                     const contracts::UiShapeDesc* desc);
QolResult ui_shape_draw_circle(const contracts::QolUiAdapter* adapter,
                               const contracts::UiShapeDesc* desc);

}  // namespace qol::features::core_ui
