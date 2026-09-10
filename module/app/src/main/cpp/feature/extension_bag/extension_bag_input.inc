void* touch_moving_item_control_locked() {
    if (g_base == 0) return nullptr;
    uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    void* control = *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL);
    if (control == nullptr) control = *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_SRC_CTRL);
    if (control == nullptr || fn_control_object_get_data == nullptr) return nullptr;
    void* data = fn_control_object_get_data(control);
    if (data == nullptr || *reinterpret_cast<void**>(data) == nullptr) return nullptr;
    if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) == 0) {
        return nullptr;
    }
    return control;
}

void reset_drag_state_locked(void* moving_control) {
    if (g_base != 0) {
        uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
        std::memset(state, 0, TOUCH_STATE_PREFIX_SIZE);
        *reinterpret_cast<void**>(state + TOUCH_STATE_ACTIVE_CTRL) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_MOVE_ON_CTRL) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_RELEASE_CTRL) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_SRC_CTRL) = nullptr;
    }
    if (moving_control != nullptr && fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(moving_control);
        if (data != nullptr) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) = 0;
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_ON_FLAG) = 0;
        }
    }
}

void clear_stale_projected_moving_locked() {
    if (g_base == 0) return;
    uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    void* moving = *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL);
    void* drop_source = *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_SRC_CTRL);
    const auto& session = g_extension_drag_session;
    const bool phase_retains_moving =
        session.phase == virtual_bag::DragPhase::kPressed ||
        session.phase == virtual_bag::DragPhase::kNativeMoving ||
        session.phase == virtual_bag::DragPhase::kTargetResolved ||
        session.phase == virtual_bag::DragPhase::kTransactionInFlight;
    void* live_root = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
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

    *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL) = nullptr;
    const auto clear_control_flags = [](void* control) {
        if (control == nullptr || fn_control_object_get_data == nullptr) return;
        void* data = fn_control_object_get_data(control);
        if (data == nullptr) return;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) = 0;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_ON_FLAG) = 0;
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
        if (g_base != 0) {
            uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            if (captured_moving_control == nullptr) {
                captured_moving_control =
                    *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL);
            }
            *reinterpret_cast<int64_t*>(state + TOUCH_STATE_RELEASE_INPUT_X) = rel_x;
            *reinterpret_cast<int64_t*>(state + TOUCH_STATE_RELEASE_INPUT_Y) = rel_y;
            *reinterpret_cast<uint64_t*>(state + TOUCH_STATE_RELEASE_INPUT_PARAM) = release_param2;
        }
    }

    // TouchHandle_ResetMovingControl performs the original 0x08 dispatch and
    // relative-coordinate restoration. Both can enter arbitrary game UI code.
    if (fn_touch_handle_reset_moving_control != nullptr) {
        fn_touch_handle_reset_moving_control();
    } else {
        VIRTBAG_LOG("ERROR release_cleanup missing ResetMovingControl ctl=%p",
                    captured_moving_control);
    }
    if (!handled && fn_touch_handle_reset_selected_control != nullptr) {
        fn_touch_handle_reset_selected_control();
    }

    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_base != 0) {
            uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_EVENT) = nullptr;
        }
        VIRTBAG_LOG("release_cleanup ctl=%p flags_cleared", captured_moving_control);
    }
}

void clear_original_item_selection_locked() {
    if (g_base == 0 || fn_ctrl_get_child == nullptr || fn_control_object_get_data == nullptr) return;
    void* item_list = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    if (item_list == nullptr) return;
    for (uint32_t index = 0; index < virtual_bag::kSlotCount; ++index) {
        void* control = fn_ctrl_get_child(item_list, index);
        if (control == nullptr) continue;
        void* data = fn_control_object_get_data(control);
        if (data == nullptr) continue;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) = 0;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_ON_FLAG) = 0;
    }
}

int original_item_category_locked(void* item) {
    if (item == nullptr) return 0;
    const uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    return (flags >> 6) & 0x3FF;
}

int drag_source_category_locked() {
    const auto& session = g_extension_drag_session;
    if (session.phase != virtual_bag::DragPhase::kIdle &&
        virtual_bag::valid_index(session.source_bag) &&
        session.source_slot >= 0 && session.source_slot < virtual_bag::kSlotCount) {
        return g_virtual_bag_state.items[session.source_bag][session.source_slot].category;
    }
    void* moving = touch_moving_item_control_locked();
    if (moving == nullptr || fn_control_object_get_data == nullptr) return 0;
    void* data = fn_control_object_get_data(moving);
    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    int module_bag = -1;
    int module_slot = -1;
    if (item != nullptr && module_slot_of_item_locked(item, &module_bag, &module_slot)) {
        return g_virtual_bag_state.items[module_bag][module_slot].category;
    }
    return original_item_category_locked(item);
}
