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
        std::memset(state, 0, 0x10);
        *reinterpret_cast<void**>(state + 0x10) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL) = nullptr;
        *reinterpret_cast<void**>(state + 0x38) = nullptr;
        *reinterpret_cast<void**>(state + 0x50) = nullptr;
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
