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

void savebackup_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    UiRect r = settings_savebackup_rect();
    ui_custom::draw_button(ctrl, {0, 0, r.w, r.h}, 0xFF2A2018,
                           UI_GOLD, 2, "存档备份 →", UI_OPTION_TEXT, 0x10, 0x0C);
}

// ---- 面板控件创建与回调 ----

