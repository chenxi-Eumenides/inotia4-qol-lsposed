void row_btn_draw(void* ctrl) {
    if (g_base == 0) return;
    bool enabled = false;
    for (int i = 0; i < kSettingsItemCount; ++i) {
        if (ctrl != g_rows[i].btn) continue;
        enabled = strcmp(g_row_status[i], "开") == 0;
        break;
    }
    if (!ui_original::draw_toggle(ctrl, enabled)) {
        ui_custom::draw_button(ctrl, {0, 0, ROW_BTN_W, ROW_BTN_H},
                               enabled ? 0xFF655444 : 0xFF241C18, UI_GOLD, 2,
                               "切换", enabled ? 0xFFFFFFFF : UI_GOLD, 8, 6);
    }
}

void row_desc_draw(void* ctrl) {
    if (g_base == 0) return;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctrl) + CO_DATA);
    if (data == nullptr || data[0] == 0) return;
    ui_draw_text(ctrl, 0, 0x8, UI_GOLD);
}

// 顶部版本标题（"QoL模块 vX.Y.Z"）：与 settings_page_indicator_draw 同款水平居中绘制
// （align=2 以 x 为中心），x 取控件绝对矩形中心，垂直偏移沿用原标题文字的 0x18。
void header_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    if (fn_grpx_set_font_color_rgb == nullptr || fn_grpx_draw_string_with_font == nullptr) return;
    uint8_t* data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctrl) + CO_DATA);
    if (data == nullptr || data[0] == 0) return;
    int64_t x = 0;
    int64_t y = 0;
    for (void* current = ctrl; current != nullptr;) {
        uint8_t* current_data = static_cast<uint8_t*>(current);
        x += *reinterpret_cast<int64_t*>(current_data + CO_RECT_X);
        y += *reinterpret_cast<int64_t*>(current_data + CO_RECT_Y);
        current = *reinterpret_cast<void**>(current_data + CO_PARENT);
    }
    x += *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(ctrl) + CO_RECT_W) / 2;
    fn_grpx_set_font_color_rgb(static_cast<int32_t>(UI_GOLD & 0xff),
                               static_cast<int32_t>((UI_GOLD >> 8) & 0xff),
                               static_cast<int32_t>((UI_GOLD >> 16) & 0xff));
    fn_grpx_draw_string_with_font(reinterpret_cast<char*>(data), static_cast<int>(x),
                                  static_cast<int>(y) + 0x18, 2, 1);
}

void back_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    if (g_option_images_loaded && ui_original::draw_back_button(ctrl, g_back_pressed)) return;
    ui_custom::draw_button(ctrl, {0, 0, BACK_BTN_W, BACK_BTN_H}, 0xFF241C18,
                           UI_GOLD, 2, "返回", UI_OPTION_TEXT, 8, 0x18);
}

// 原版物品详情「使用」按钮图组：UIDesc_DrawMenuButton 用 UI_GetButtonImgGroup(DrawID=0)
// 取当前语言按钮图组（一次只加载当前语言；基址 eng/kor/zhc/zht/jpn/fra/ger =
// 0x29/0x33/0x38/0x2e/0x3d/0x42/0x24），再取 loc 0x0e。
// UI_GetButtonImgGroup 未登记符号，改为探测这些单元中已加载且含 loc 0x0e 的那个。
int32_t settings_use_button_unit() {
    static const int32_t kCandidates[] = {0x29, 0x33, 0x38, 0x2e, 0x3d, 0x42, 0x24};
    if (fn_imgsys_get_group == nullptr || fn_imgsys_get_loc == nullptr) return -1;
    for (int32_t unit : kCandidates) {
        if (fn_imgsys_get_group(unit) != nullptr &&
            fn_imgsys_get_loc(unit, USE_BUTTON_LOC) != nullptr) {
            return unit;
        }
    }
    return -1;
}

void savebackup_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    // 优先复用原版「使用」贴图（按原始尺寸居中绘制，与开关贴图同为原始尺寸）。
    if (g_base != 0) {
        const int32_t unit = settings_use_button_unit();
        if (unit >= 0 && ui_draw_control_image_part_centered(ctrl, unit, USE_BUTTON_LOC, 2, 1)) {
            return;
        }
    }
    // 退化路径：图组未就绪时自绘同风格按钮 + 文字「使用」。
    ui_custom::draw_button(ctrl, {0, 0, SAVEBACKUP_BTN_W, ROW_BTN_H}, 0xFF2A2018,
                           UI_GOLD, 2, nullptr, 0, 0, 0);
    ui_draw_text_centered(ctrl, 0x8, UI_OPTION_TEXT);
}

void page_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    const bool is_next = ctrl == g_next_page_btn;
    const bool is_prev = ctrl == g_prev_page_btn;
    if (!is_next && !is_prev) return;
    uint8_t* button_data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctrl) + CO_DATA);
    const bool pressed = button_data != nullptr &&
                         *reinterpret_cast<uint8_t*>(button_data + CB_STATE) == 1;
    const int32_t loc = is_next
        ? (pressed ? PAGE_NEXT_ARROW_PRESSED_LOC : PAGE_NEXT_ARROW_NORMAL_LOC)
        : (pressed ? PAGE_PREV_ARROW_PRESSED_LOC : PAGE_PREV_ARROW_NORMAL_LOC);
    // UIOption 的语言切换箭头来自同一 0x59 图像单元；按按钮状态选择原版正常/按下分片，
    // 保留原始分片尺寸与透明边缘，不再叠加自绘矩形或文字。分页边界仍由事件处理钳制，
    // 原版资源没有单独禁用态。
    if (g_option_images_loaded &&
        ui_draw_control_image_part_centered(ctrl, OPTION_IMAGE_UNIT, loc, 2, 1)) {
        return;
    }
    if (!g_page_arrow_unavailable_logged) {
        QOL_LOG_DEBUG(QolDomain::kUi, "settings page arrow image unavailable: unit=0x%x", OPTION_IMAGE_UNIT);
        g_page_arrow_unavailable_logged = true;
    }
}

void settings_page_indicator_draw(const UiRect& root_rect) {
    if (fn_grpx_set_font_color_rgb == nullptr || fn_grpx_draw_string_with_font == nullptr) return;
    char page_text[16];
    snprintf(page_text, sizeof(page_text), "%d/%d", g_page_index + 1,
             g_total_pages > 0 ? g_total_pages : 1);
    fn_grpx_set_font_color_rgb(static_cast<int32_t>(UI_OPTION_TEXT & 0xff),
                               static_cast<int32_t>((UI_OPTION_TEXT >> 8) & 0xff),
                               static_cast<int32_t>((UI_OPTION_TEXT >> 16) & 0xff));
    const UiRect indicator = settings_page_indicator_rect();
    fn_grpx_draw_string_with_font(
        page_text, static_cast<int>(root_rect.x + ROOT_W / 2),
        static_cast<int>(root_rect.y + indicator.y + 0x04), 2, 1);
}

// ---- 面板控件创建与回调 ----
