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

