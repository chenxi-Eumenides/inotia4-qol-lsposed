void row_btn_draw(void* ctrl) {
    if (g_base == 0) return;
    bool enabled = false;
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
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

void address_draw(void* ctrl) {
    ui_draw_text(ctrl, 0, 0x18, UI_GOLD);
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

// ---- 面板控件创建与回调 ----

