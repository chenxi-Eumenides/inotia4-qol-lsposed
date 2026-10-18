#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/extension_bag_logging.h"

void* touch_moving_item_control_locked() {
    void* moving = nullptr;
    void* drop_source = nullptr;
    int64_t release_x = 0;
    int64_t release_y = 0;
    if (!extension_bag_ui_touch_state(&moving, &drop_source, &release_x, &release_y)) return nullptr;
    void* control = moving != nullptr ? moving : drop_source;
    if (control == nullptr) return nullptr;
    if (extension_bag_ui_data(control) == nullptr || extension_bag_ui_item(control) == nullptr ||
        !extension_bag_ui_control_is_moving(control)) return nullptr;
    return control;
}

void reset_drag_state_locked(void* moving_control) {
    (void)extension_bag_ui_clear_touch_state();
    if (moving_control != nullptr) (void)extension_bag_ui_clear_control_drag_flags(moving_control);
}

void clear_stale_projected_moving_locked() {
    void* moving = nullptr;
    void* drop_source = nullptr;
    int64_t release_x = 0;
    int64_t release_y = 0;
    if (!extension_bag_ui_touch_state(&moving, &drop_source, &release_x, &release_y)) return;
    if (moving == nullptr && drop_source == nullptr) return;
    const auto& session = g_extension_drag_session;
    // 会话空闲且非模块触摸捕获时，MOVING_CTRL/DROP_SRC_CTRL 属于原版 TouchHandle
    // 手势（释放时由 TouchHandle_ResetMovingControl 派发 event 8）。模块在此时清除会
    // 打断原版触摸状态机、导致 0x18 释放事件不再产生——故空闲态绝不触碰。
    if (session.phase == virtual_bag::DragPhase::kIdle && !g_extension_touch_capture) return;
    const bool phase_retains_moving =
        session.phase == virtual_bag::DragPhase::kPressed ||
        session.phase == virtual_bag::DragPhase::kNativeMoving ||
        session.phase == virtual_bag::DragPhase::kTargetResolved ||
        session.phase == virtual_bag::DragPhase::kTransactionInFlight;
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
    const bool source_slot_valid = virtual_bag::valid_index(session.source_bag) &&
                                   session.source_slot >= 0 &&
                                   session.source_slot < virtual_bag::kSlotCount;
    const bool moving_matches_source =
        live_root == g_projected_item_root && g_projected_item_root != nullptr &&
        source_slot_valid && moving == valid_child_locked(g_projected_item_root, session.source_slot);
    const bool retain = phase_retains_moving && g_module_view_installed &&
        g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        g_module_view_index == session.source_bag &&
        session.view_generation == g_extension_tab_generation &&
        g_extension_tab_generation == g_inventory_generation &&
        moving_matches_source;
    if (retain) return;

    const auto clear_control_flags = [](void* control) {
        if (control == nullptr) return;
        (void)extension_bag_ui_clear_control_drag_flags(control);
    };
    clear_control_flags(moving);
    if (drop_source != moving) clear_control_flags(drop_source);
    VIRTBAG_LOG("stale projected moving cleared moving=%p drop_source=%p phase=%d view=%d/%d "
                "generation=%llu/%llu/%llu root=%p projected_root=%p",
                moving, drop_source, static_cast<int>(session.phase), g_module_view_index,
                session.source_bag, static_cast<unsigned long long>(session.view_generation),
                static_cast<unsigned long long>(g_extension_tab_generation),
                static_cast<unsigned long long>(g_inventory_generation), live_root,
                g_projected_item_root);
}

void complete_original_release_cleanup_locked(void* moving_control, int64_t rel_x, int64_t rel_y,
                                               uint64_t release_param2, bool handled) {
    void* captured_moving_control = moving_control;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (captured_moving_control == nullptr) {
            void* drop_source = nullptr;
            int64_t ignored_x = 0;
            int64_t ignored_y = 0;
            (void)extension_bag_ui_touch_state(&captured_moving_control, &drop_source,
                                               &ignored_x, &ignored_y);
        }
        (void)extension_bag_ui_set_touch_release_input(rel_x, rel_y, release_param2);
    }

    // TouchHandle_ResetMovingControl performs the original 0x08 dispatch and
    // relative-coordinate restoration. Both can enter arbitrary game UI code.
    const auto* ui = g_sdk == nullptr ? nullptr : g_sdk->ui;
    if (ui != nullptr && ui->reset_touch_moving != nullptr) {
        (void)ui->reset_touch_moving();
    } else {
        VIRTBAG_LOG_ERROR("ERROR release_cleanup missing ResetMovingControl ctl=%p",
                    captured_moving_control);
    }
    if (!handled && ui != nullptr && ui->reset_touch_selected != nullptr) {
        (void)ui->reset_touch_selected();
    }

    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        (void)extension_bag_ui_clear_touch_drop_event();
        VIRTBAG_LOG("release_cleanup ctl=%p flags_cleared", captured_moving_control);
    }
}

void clear_original_item_selection_locked() {
    void* item_list = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
    if (item_list == nullptr) return;
    for (uint32_t index = 0; index < virtual_bag::kSlotCount; ++index) {
        void* control = extension_bag_ui_child_at(item_list, static_cast<int>(index));
        if (control == nullptr) continue;
        (void)extension_bag_ui_clear_control_drag_flags(control);
    }
}

int original_item_category_locked(void* item) {
    if (item == nullptr) return 0;
    int category = 0;
    return read_item_category(item, &category) ? category : 0;
}

int drag_source_category_locked() {
    const auto& session = g_extension_drag_session;
    if (session.phase != virtual_bag::DragPhase::kIdle &&
        virtual_bag::valid_index(session.source_bag) &&
        session.source_slot >= 0 && session.source_slot < virtual_bag::kSlotCount) {
        return g_virtual_bag_state.items[session.source_bag][session.source_slot].category;
    }
    void* moving = touch_moving_item_control_locked();
    if (moving == nullptr) return 0;
    void* data = extension_bag_ui_data(moving);
    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    int module_bag = -1;
    int module_slot = -1;
    if (item != nullptr && module_slot_of_item_locked(item, &module_bag, &module_slot)) {
        return g_virtual_bag_state.items[module_bag][module_slot].category;
    }
    return original_item_category_locked(item);
}
