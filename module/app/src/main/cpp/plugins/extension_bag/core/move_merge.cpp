#include "plugins/extension_bag/core/move_merge.h"

#include "plugins/extension_bag/core/move_merge_hook.h"
#include "plugins/extension_bag/core/ui_state.h"

// 拖拽合并开关已随 feature 迁入本插件：不再 require core.item 的 move_merge.control
// 能力，直接读 EB 本地 feature 状态与 ptrslot 就绪态（与迁移前 capability
// enabled = feature_enabled && wrapper_ready 的语义一致）。
bool move_merge_enabled() {
    return qol::features::extension_bag::merge::enabled() &&
           qol::features::extension_bag::merge::wrapper_ready();
}

bool set_extension_source_protection_enabled(bool enabled) {
    qol::extension_bag::ui_state().aux.source_protection_requested.store(
        enabled, std::memory_order_release);
    return true;
}
