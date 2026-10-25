#pragma once

#include "plugins/extension_bag/runtime/state.h"

namespace qol::features::extension_bag::core {

inline virtual_bag::DragTargetKind classify_drag_target(int bag, int slot,
                                                         bool extension_tab = false,
                                                         bool cancelled = false) {
    using virtual_bag::DragTargetKind;
    if (cancelled) return DragTargetKind::kCancel;
    if (extension_tab) return DragTargetKind::kExternalTarget;
    if (bag == virtual_bag::kOriginalTaskBag) return DragTargetKind::kTaskBagRejected;
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return DragTargetKind::kInvalid;
    if (virtual_bag::valid_original_transaction_bag(bag)) return DragTargetKind::kOriginalBag;
    if (virtual_bag::valid_extension_logical_bag(bag)) return DragTargetKind::kExtensionSlot;
    return DragTargetKind::kInvalid;
}

}  // namespace qol::features::extension_bag::core

// 原生拖拽会话：投影拖动、moving 控件、释放清理与来源类别判定。
// 只处理 UI/会话状态，不直接提交物品事务（提交由 transaction/drop.cpp 调用）。
void begin_projected_drag_session_locked(int source_slot);
void advance_projected_drag_session_locked();
bool projected_drag_session_active_locked();
void cancel_projected_drag_session_locked(const char* reason);
void* touch_moving_item_control_locked();
void reset_drag_state_locked(void* moving_control);
void clear_stale_projected_moving_locked();
void complete_original_release_cleanup_locked(void* moving_control, int64_t rel_x, int64_t rel_y,
                                              uint64_t release_param2, bool handled);
void clear_original_item_selection_locked();
int drag_source_category_locked();
