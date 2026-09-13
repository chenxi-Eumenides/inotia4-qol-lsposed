void settings_panel_enter() {
    SETTINGS_LOG("settings panel enter");
    if (g_root != nullptr) {
        char json[512] = {0};
        if (jni_get_config_json(json, sizeof(json))) {
            parse_config_into_rows(json);
        }
        if (!g_option_images_loaded) {
            g_option_images_loaded = ui_original::load_option_images();
        }
        if (!g_title_background_images_loaded) {
            g_title_background_images_loaded = ui_load_image_unit(TITLE_BACKGROUND_LEFT_UNIT) &&
                ui_load_image_unit(TITLE_BACKGROUND_RIGHT_UNIT);
        }
        g_panel_active = true;
        g_close_delay_frames = 0;
        g_back_pressed = false;
        return;
    }
    if (g_base == 0 || fn_calc_res_width == nullptr || fn_calc_res_height == nullptr) {
        SETTINGS_LOG("settings panel enter: symbols not resolved");
        return;
    }
    // 面板打开时从 Kotlin 拉取当前配置
    char json[512] = {0};
    if (jni_get_config_json(json, sizeof(json))) {
        parse_config_into_rows(json);
    }
    g_option_images_loaded = ui_original::load_option_images();
    g_title_background_images_loaded = ui_load_image_unit(TITLE_BACKGROUND_LEFT_UNIT) &&
        ui_load_image_unit(TITLE_BACKGROUND_RIGHT_UNIT);
    void* root = ui_create_root(settings_root_rect());
    if (root == nullptr) {
        SETTINGS_LOG("settings panel enter: root create failed");
        return;
    }
    UiRect root_rect{};
    ui_get_rect(root, &root_rect);
    g_back_btn = ui_create_button(root, {8 - root_rect.x, 8 - root_rect.y, BACK_BTN_W, BACK_BTN_H}, "返回",
                                  &settings_back_clicked, &back_btn_draw);
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        UiRect label = settings_label_rect(i);
        void* btn = ui_create_button(root, settings_toggle_rect(i), "切换",
                                     &settings_row_clicked, &row_btn_draw);
        void* desc = ui_create_label(root, label, kRowLabels[i], &row_desc_draw);
        if (btn == nullptr || desc == nullptr) continue;
        g_rows[i].btn = btn;
        g_rows[i].desc = desc;
    }
    void* addr = ui_create_label(root,
                                   {ROOT_W / 2 - 0x38, ADDR_Y, 0x120, ADDR_H},
                                   g_addr_text, &address_draw);
    if (addr != nullptr) {
        g_addr_desc = addr;
    }
    // 存档备份单元格（v0.7.x 改造）：复用网格末行空位，
    // 左描述与配置项同款，右侧「使用」样式按钮（右缘与其它开关对齐）—— push 打开备份面板
    void* sb_desc = ui_create_label(root, settings_label_rect(SETTINGS_ROW_COUNT),
                                    "存档备份", &row_desc_draw);
    g_savebackup_desc = sb_desc;
    void* sb_btn = ui_create_button(root, settings_savebackup_rect(), "使用",
                                    &settings_savebackup_clicked, &savebackup_btn_draw);
    g_savebackup_btn = sb_btn;
    g_root = root;
    g_panel_active = true;
    g_close_delay_frames = 0;
    g_back_pressed = false;
    SETTINGS_LOG("settings panel enter: root=%p %d settings cells + backup cell + addr created",
                 root, SETTINGS_ROW_COUNT);
}

void settings_panel_process() {
    if (!g_panel_active || g_root == nullptr || g_base == 0) return;
    if (g_close_delay_frames > 0) {
        --g_close_delay_frames;
        if (g_close_delay_frames == 0 && fn_ui_set_popup_process_info != nullptr) {
            fn_ui_set_popup_process_info(3, 0);
            return;
        }
    }
    ui_begin_frame();
    UiRect root_rect{};
    if (!ui_get_rect(g_root, &root_rect)) return;
    const bool background_drawn = g_title_background_images_loaded &&
        ui_draw_image_part(TITLE_BACKGROUND_LEFT_UNIT, TITLE_BACKGROUND_LOC,
                           static_cast<int32_t>(root_rect.x + ROOT_W / 2 - 0x12c),
                           static_cast<int32_t>(root_rect.y + ROOT_H / 2), 2, 1) &&
        ui_draw_image_part(TITLE_BACKGROUND_RIGHT_UNIT, TITLE_BACKGROUND_LOC,
                           static_cast<int32_t>(root_rect.x + ROOT_W / 2 + 0x12c),
                           static_cast<int32_t>(root_rect.y + ROOT_H / 2), 2, 1);
    if (!background_drawn && !g_background_unavailable_logged) {
        SETTINGS_LOG("settings background image unavailable; using restored LCD");
        g_background_unavailable_logged = true;
    }
    UiRect screen_rect = {0, 0, ROOT_W + root_rect.x * 2, ROOT_H};
    ui_fill_rect_alpha(screen_rect, 0xFF000000, 0x50);
    int64_t separators[VISIBLE_GRID_ROWS - 1] = {};
    for (int i = 1; i < VISIBLE_GRID_ROWS; ++i) {
        separators[i - 1] = root_rect.y + GRID_Y + i * CELL_H;
    }
    UiRect grid_rect = {root_rect.x + CONTENT_X, root_rect.y + GRID_Y, CONTENT_W, GRID_H};
    ui_draw_panel_decor(grid_rect, separators, VISIBLE_GRID_ROWS - 1, UI_GOLD);
    for (int i = 0; i <= SETTINGS_COLUMN_COUNT; ++i) {
        ui_draw_vertical_line(root_rect.x + CONTENT_X + i * CELL_W, root_rect.y + GRID_Y,
                              GRID_H, UI_GOLD, 3);
    }
    if (g_back_btn != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_back_btn);
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        if (g_rows[i].btn != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_rows[i].btn);
        if (g_rows[i].desc != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_rows[i].desc);
    }
    if (g_addr_desc != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_addr_desc);
    if (g_savebackup_desc != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_savebackup_desc);
    if (g_savebackup_btn != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_savebackup_btn);
    ui_end_frame();
}

void settings_panel_f3() {
    SETTINGS_LOG("settings panel f3 (terminate)");
    g_panel_active = false;
    g_close_delay_frames = 0;
    g_back_pressed = false;
    if (g_option_images_loaded) {
        ui_original::unload_option_images();
        g_option_images_loaded = false;
    }
}

void settings_panel_f4() {}

void settings_row_clicked(void* ctrl) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        if (ctrl != g_rows[i].btn) continue;
        if (!jni_toggle_config(kRowKeys[i])) return;
        char json[512] = {0};
        if (jni_get_config_json(json, sizeof(json))) {
            parse_config_into_rows(json);
            return;
        }
        const bool target_enabled = strcmp(g_row_status[i], "关") == 0;
        snprintf(g_row_status[i], CB_TEXT_SIZE, "%s", target_enabled ? "开" : "关");
        SETTINGS_LOG("settings row%d toggled: %s", i, g_row_status[i]);
        return;
    }
}

void settings_back_clicked(void* ctrl) {
    SETTINGS_LOG("settings back pressed");
    g_back_pressed = true;
}

uint32_t settings_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (g_root == nullptr || param == 0) return 1;
    if (g_close_delay_frames > 0) return 1;
    if (event == 0x17) {
        int64_t tx = *reinterpret_cast<const int64_t*>(param);
        int64_t ty = *reinterpret_cast<const int64_t*>(param + 8);
        for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
            UiRect toggle = settings_toggle_rect(i);
            if (g_rows[i].btn != nullptr &&
                ui_hit_test(g_rows[i].btn, tx, ty, {0, 0, toggle.w, toggle.h})) {
                SETTINGS_LOG("settings event: row%d toggle hit tx=%lld ty=%lld rect=(%lld,%lld)",
                               i, static_cast<long long>(tx), static_cast<long long>(ty),
                               static_cast<long long>(toggle.x), static_cast<long long>(toggle.y));
                settings_row_clicked(g_rows[i].btn);
                break;
            }
        }
        if (g_savebackup_btn != nullptr &&
            ui_hit_test(g_savebackup_btn, tx, ty, settings_savebackup_rect())) {
            SETTINGS_LOG("settings event: savebackup btn hit");
            settings_savebackup_clicked(g_savebackup_btn);
            return 1;
        }
        if (g_back_btn != nullptr &&
            ui_hit_test(g_back_btn, tx, ty, {0, 0, BACK_BTN_W, BACK_BTN_H})) {
            settings_back_clicked(g_back_btn);
            return 1;
        }
    } else if (event == 0x18 && g_back_pressed) {
        int64_t tx = *reinterpret_cast<const int64_t*>(param);
        int64_t ty = *reinterpret_cast<const int64_t*>(param + 8);
        const bool released_on_back = g_back_btn != nullptr &&
            ui_hit_test(g_back_btn, tx, ty, {0, 0, BACK_BTN_W, BACK_BTN_H});
        g_back_pressed = false;
        if (released_on_back) {
            SETTINGS_LOG("settings back released -> schedule close");
            g_close_delay_frames = 2;
        }
    }
    return 1;
}

void settings_savebackup_clicked(void* ctrl) {
    SETTINGS_LOG("settings savebackup button clicked");
    // 嵌套 push：保持设置页在栈中第 N 位；备份面板成为新栈顶；
    // 关闭备份面板后回到设置页（实测如 push 行为异常，退化为切换式：先关闭自身再开备份面板）。
    const std::string r = data_savebackup_ui_open_panel();
    if (r.find("\"ok\":true") == std::string::npos) {
        SETTINGS_LOG("savebackup open failed: %s", r.c_str());
    }
}

// ---- 主菜单「更多游戏」按钮钩子 ----

