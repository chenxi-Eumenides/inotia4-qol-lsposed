#pragma once

#include <cstdint>

#include "plugins/extension_bag/core/geometry.h"

namespace qol::features::extension_bag {

// 三个页签宿主（背包/商店/合成）共用的 ensure 门控。
// 签名（root + 子控件数 + 业务计数）不变时每 10 帧做一次完整 ensure；
// 其余帧只逐个证明已挂载按钮存活，任一失效立即回到完整路径。
struct TabEnsureGate {
    void* root = nullptr;
    int child_count = -1;
    uint64_t extra = 0;
    uint32_t tick = 0;
};

inline bool tab_ensure_can_skip(TabEnsureGate& gate, void* root, int child_count,
                                uint64_t extra, void* const* buttons, int visible) {
    const bool unchanged = gate.root == root && gate.child_count == child_count &&
                           gate.extra == extra;
    if (unchanged && (++gate.tick % 10) != 0) {
        for (int index = 0; index < visible; ++index) {
            void* button = buttons[index];
            if (button != nullptr && extension_bag_ui_user_type(button) < 0) return false;
        }
        return true;
    }
    gate.root = root;
    gate.child_count = child_count;
    gate.extra = extra;
    gate.tick = 0;
    return false;
}

}  // namespace qol::features::extension_bag
