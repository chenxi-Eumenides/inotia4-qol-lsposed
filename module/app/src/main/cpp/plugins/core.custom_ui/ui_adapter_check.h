#pragma once

#include <cstdint>

#include "../../contracts/ui.h"

namespace qol::features::core_ui {

// UI Adapter 契约基线。组件绑定校验与形状绘制共用同一组判定，
// 保证 minor 下限只有一个来源。
inline constexpr uint16_t kUiAdapterBaseMinor = 13;

inline bool ui_adapter_ready(const contracts::QolUiAdapter* adapter) {
    return adapter != nullptr && adapter->major == contracts::kUiAdapterMajor &&
        adapter->minor >= kUiAdapterBaseMinor;
}

inline bool ui_adapter_on_main_thread(const contracts::QolUiAdapter* adapter) {
    return ui_adapter_ready(adapter) && adapter->is_main_thread != nullptr &&
        adapter->is_main_thread() != 0;
}

}  // namespace qol::features::core_ui
