#pragma once

#include <cstdint>

#include "../../contracts/ui.h"

namespace qol::features::core_ui {

// 矩形必须落在 int32 坐标内、宽高为正，且 x+w / y+h 不溢出 int32。
// 组件创建与形状绘制共用同一判定，避免两份实现漂移。
inline bool rect_within_int32(UiRect rect) {
    return rect.x >= INT32_MIN && rect.y >= INT32_MIN && rect.x <= INT32_MAX &&
        rect.y <= INT32_MAX && rect.w > 0 && rect.h > 0 && rect.w <= INT32_MAX &&
        rect.h <= INT32_MAX && rect.x <= static_cast<int64_t>(INT32_MAX) - rect.w &&
        rect.y <= static_cast<int64_t>(INT32_MAX) - rect.h;
}

}  // namespace qol::features::core_ui
