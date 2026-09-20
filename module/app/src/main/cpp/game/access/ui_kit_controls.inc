void ui_set_rect(void* ctrl, UiRect rect) {
    if (ctrl == nullptr) return;
    uint8_t* data = static_cast<uint8_t*>(ctrl);
    *reinterpret_cast<int64_t*>(data + CO_RECT_X) = rect.x;
    *reinterpret_cast<int64_t*>(data + CO_RECT_Y) = rect.y;
    *reinterpret_cast<int64_t*>(data + CO_RECT_W) = rect.w;
    *reinterpret_cast<int64_t*>(data + CO_RECT_H) = rect.h;
}

bool ui_get_rect(void* ctrl, UiRect* rect) {
    if (ctrl == nullptr || rect == nullptr) return false;
    uint8_t* data = static_cast<uint8_t*>(ctrl);
    rect->x = *reinterpret_cast<int64_t*>(data + CO_RECT_X);
    rect->y = *reinterpret_cast<int64_t*>(data + CO_RECT_Y);
    rect->w = *reinterpret_cast<int64_t*>(data + CO_RECT_W);
    rect->h = *reinterpret_cast<int64_t*>(data + CO_RECT_H);
    return true;
}

void* ui_create_root(UiRect rect) {
    if (g_base == 0 || fn_ctrl_create == nullptr || fn_calc_res_width == nullptr ||
        fn_calc_res_height == nullptr) {
        return nullptr;
    }
    void* root = fn_ctrl_create(0, nullptr, nullptr, nullptr);
    if (root == nullptr) return nullptr;
    ui_set_rect(root, rect);
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(root) + CO_TYPE) = 0;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(root) + CO_ACTIVE) = 0x20;
    *reinterpret_cast<uintptr_t*>(static_cast<uint8_t*>(root) + CO_PROC) =
        g_base + F_TOUCH_HANDLE_CONTROL_EVENT_PROC_VMA;
    set_control_proc(root, 0);
    return root;
}

void* ui_create_button(void* parent, UiRect rect, const char* text,
                       UiClickProc on_click, UiDrawProc draw_proc) {
    if (parent == nullptr || fn_ctrl_btn_create == nullptr || fn_ctrl_btn_set_text == nullptr ||
        fn_ctrl_set_active == nullptr) {
        return nullptr;
    }
    void* ctrl = fn_ctrl_btn_create(parent, nullptr);
    if (ctrl == nullptr) return nullptr;
    ui_set_rect(ctrl, rect);
    if (text != nullptr) fn_ctrl_btn_set_text(ctrl, const_cast<char*>(text));
    if (draw_proc != nullptr && fn_ctrl_btn_set_draw_proc != nullptr) {
        fn_ctrl_btn_set_draw_proc(ctrl, reinterpret_cast<void*>(draw_proc));
    }
    fn_ctrl_set_active(ctrl, 0x20);
    set_control_proc(ctrl, g_base + F_CONTROL_BUTTON_CONTROL_EVENT_PROC_VMA);
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ctrl) + CO_EVENT_CALL_TYPE) = 0x200;
    set_execute_proc(ctrl, on_click);
    return ctrl;
}

void* ui_create_label(void* parent, UiRect rect, const char* text, UiDrawProc draw_proc) {
    if (parent == nullptr || fn_ctrl_btn_create == nullptr || fn_ctrl_btn_set_text == nullptr ||
        fn_ctrl_set_active == nullptr) {
        return nullptr;
    }
    void* ctrl = fn_ctrl_btn_create(parent, nullptr);
    if (ctrl == nullptr) return nullptr;
    ui_set_rect(ctrl, rect);
    if (text != nullptr) fn_ctrl_btn_set_text(ctrl, const_cast<char*>(text));
    if (draw_proc != nullptr && fn_ctrl_btn_set_draw_proc != nullptr) {
        fn_ctrl_btn_set_draw_proc(ctrl, reinterpret_cast<void*>(draw_proc));
    }
    fn_ctrl_set_active(ctrl, 0);
    return ctrl;
}

void* find_child_in_area(void* parent, UiRect area, int depth) {
    if (parent == nullptr || depth > 4 || fn_ctrl_get_count == nullptr || fn_ctrl_get_child == nullptr) {
        return nullptr;
    }
    uint32_t count = fn_ctrl_get_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        void* child = fn_ctrl_get_child(parent, i);
        UiRect rect{};
        if (child == nullptr || !ui_get_rect(child, &rect)) continue;
        uint8_t* data = static_cast<uint8_t*>(child);
        bool is_button = *reinterpret_cast<uint32_t*>(data + CO_TYPE) == 3 &&
                         *reinterpret_cast<void**>(data + CO_DATA) != nullptr;
        if (is_button && rect.x >= area.x && rect.y >= area.y && rect.x + rect.w <= area.x + area.w &&
            rect.y + rect.h <= area.y + area.h) {
            return child;
        }
        void* nested = find_child_in_area(child, area, depth + 1);
        if (nested != nullptr) return nested;
    }
    return nullptr;
}

void* ui_find_child_in_area(void* parent, UiRect area) {
    return find_child_in_area(parent, area, 0);
}

bool ui_clone_button_style(void* target, void* source, UiClickProc on_click) {
    if (target == nullptr || source == nullptr) return false;
    uint8_t* target_data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(target) + CO_DATA);
    uint8_t* source_data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(source) + CO_DATA);
    if (target_data == nullptr || source_data == nullptr) return false;
    std::memcpy(target_data, source_data, CB_SIZE);
    *reinterpret_cast<void**>(target_data + CB_EXECUTE_PROC) = reinterpret_cast<void*>(on_click);
    return true;
}

bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect) {
    if (ctrl == nullptr) return false;
    int64_t absolute_x = 0;
    int64_t absolute_y = 0;
    for (void* current = ctrl; current != nullptr;) {
        uint8_t* data = static_cast<uint8_t*>(current);
        absolute_x += *reinterpret_cast<int64_t*>(data + CO_RECT_X);
        absolute_y += *reinterpret_cast<int64_t*>(data + CO_RECT_Y);
        current = *reinterpret_cast<void**>(data + CO_PARENT);
    }
    return x >= absolute_x && x < absolute_x + rect.w && y >= absolute_y && y < absolute_y + rect.h;
}

void* ui_original_bag_container() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_BAG_CONTAINER_VMA);
}

void* ui_find_root(uint32_t kind) {
    if (g_base == 0) return nullptr;
    switch (kind) {
        case 0:
            return *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_BAG_CONTAINER_VMA);
        case 1:
            return *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
        case 2:
            return *reinterpret_cast<void**>(g_base + G_UISTORE_BAG_CTRL_VMA);
        case 3:
            return *reinterpret_cast<void**>(g_base + G_UISTORE_ITEM_CTRL_VMA);
        case 4:
            if (g_uimix == nullptr) return nullptr;
            return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_BAG_GROUP);
        case 5:
            if (g_uimix == nullptr) return nullptr;
            return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_ITEM_GROUP);
        default:
            return nullptr;
    }
}

bool ui_control_type(void* ctrl, int32_t* out_type) {
    if (ctrl == nullptr || out_type == nullptr) return false;
    *out_type = static_cast<int32_t>(
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ctrl) + CO_TYPE));
    return true;
}

bool ui_control_active(void* ctrl, int32_t* out_active) {
    if (ctrl == nullptr || out_active == nullptr) return false;
    *out_active = static_cast<int32_t>(
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ctrl) + CO_ACTIVE));
    return true;
}

bool ui_control_user_type(void* ctrl, int32_t* out_type) {
    if (ctrl == nullptr || out_type == nullptr || fn_control_object_get_user_type == nullptr) {
        return false;
    }
    *out_type = static_cast<int32_t>(fn_control_object_get_user_type(ctrl));
    return true;
}

void* ui_control_data(void* ctrl) {
    if (ctrl == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(ctrl) + CO_DATA);
}

void* ui_control_item(void* ctrl) {
    if (ctrl == nullptr) return nullptr;
    if (fn_control_item_get_item != nullptr) return fn_control_item_get_item(ctrl);
    void* data = ui_control_data(ctrl);
    if (data == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(data) + ITEM_CTRL_ITEM);
}

bool ui_control_item_slot(void* ctrl, int32_t* out_slot) {
    if (ctrl == nullptr || out_slot == nullptr || fn_ui_equip_get_item_slot_index == nullptr) {
        return false;
    }
    *out_slot = fn_ui_equip_get_item_slot_index(ctrl);
    return true;
}

void* ui_control_parent(void* ctrl) {
    if (ctrl == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(ctrl) + CO_PARENT);
}

bool ui_execute_proc_matches(void* ctrl, UiClickProc expected) {
    if (ctrl == nullptr || expected == nullptr) return false;
    uint8_t* data = static_cast<uint8_t*>(ui_control_data(ctrl));
    if (data == nullptr) return false;
    return *reinterpret_cast<void**>(data + CB_EXECUTE_PROC) ==
           reinterpret_cast<void*>(expected);
}

bool ui_control_absolute_origin(void* ctrl, int64_t* out_x, int64_t* out_y) {
    if (ctrl == nullptr || out_x == nullptr || out_y == nullptr) return false;
    int64_t absolute_x = 0;
    int64_t absolute_y = 0;
    for (void* current = ctrl; current != nullptr;) {
        uint8_t* data = static_cast<uint8_t*>(current);
        absolute_x += *reinterpret_cast<int64_t*>(data + CO_RECT_X);
        absolute_y += *reinterpret_cast<int64_t*>(data + CO_RECT_Y);
        current = *reinterpret_cast<void**>(data + CO_PARENT);
    }
    *out_x = absolute_x;
    *out_y = absolute_y;
    return true;
}

bool ui_touch_state(void** out_moving, void** out_drop_source, int64_t* out_x, int64_t* out_y) {
    if (out_moving == nullptr || out_drop_source == nullptr || out_x == nullptr || out_y == nullptr ||
        g_base == 0) {
        return false;
    }
    const uint8_t* state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    *out_moving = *reinterpret_cast<void* const*>(state + TOUCH_STATE_MOVING_CTRL);
    *out_drop_source = *reinterpret_cast<void* const*>(state + TOUCH_STATE_DROP_SRC_CTRL);
    *out_x = *reinterpret_cast<const int64_t*>(state + TOUCH_STATE_RELEASE_X);
    *out_y = *reinterpret_cast<const int64_t*>(state + TOUCH_STATE_RELEASE_Y);
    return true;
}

bool ui_control_is_moving(void* ctrl, int32_t* out_moving) {
    if (ctrl == nullptr || out_moving == nullptr) return false;
    void* data = ui_control_data(ctrl);
    if (data == nullptr) return false;
    *out_moving = *reinterpret_cast<const uint8_t*>(static_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) != 0 ? 1 : 0;
    return true;
}

bool ui_clear_control_drag_flags(void* ctrl) {
    if (ctrl == nullptr) return false;
    void* data = ui_control_data(ctrl);
    if (data == nullptr) return false;
    auto* bytes = static_cast<uint8_t*>(data);
    bytes[ITEM_CTRL_MOVING_FLAG] = 0;
    bytes[ITEM_CTRL_ON_FLAG] = 0;
    return true;
}

bool ui_clear_touch_state() {
    if (g_base == 0) return false;
    auto* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    std::memset(state, 0, TOUCH_STATE_PREFIX_SIZE);
    *reinterpret_cast<void**>(state + TOUCH_STATE_ACTIVE_CTRL) = nullptr;
    *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL) = nullptr;
    *reinterpret_cast<void**>(state + TOUCH_STATE_MOVE_ON_CTRL) = nullptr;
    *reinterpret_cast<void**>(state + TOUCH_STATE_RELEASE_CTRL) = nullptr;
    *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_SRC_CTRL) = nullptr;
    return true;
}

bool ui_set_touch_release_input(int64_t x, int64_t y, uint64_t param) {
    if (g_base == 0) return false;
    auto* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    *reinterpret_cast<int64_t*>(state + TOUCH_STATE_RELEASE_INPUT_X) = x;
    *reinterpret_cast<int64_t*>(state + TOUCH_STATE_RELEASE_INPUT_Y) = y;
    *reinterpret_cast<uint64_t*>(state + TOUCH_STATE_RELEASE_INPUT_PARAM) = param;
    return true;
}

bool ui_clear_touch_drop_event() {
    if (g_base == 0) return false;
    auto* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_EVENT) = nullptr;
    return true;
}
