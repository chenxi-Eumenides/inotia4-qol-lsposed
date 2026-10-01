#pragma once

namespace qol::features::extension_bag::model {

// 主菜单清理必须边沿触发/幂等：状态持续非 5（面板打开时可能读到非 5）不得每帧重入，
// 否则每帧 reset 加载态与页签 generation，触发重建风暴。仅在状态变化或上次清理失败重试时为 true。
inline bool main_menu_cleanup_due(int previous_state, int current_state, bool retry_dirty) {
    if (current_state < 0 || current_state == 5) return false;
    return retry_dirty || previous_state != current_state;
}

}  // namespace qol::features::extension_bag::model
