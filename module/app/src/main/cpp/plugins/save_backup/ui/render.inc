// save_backup 面板绘制全部通过 core.custom_ui 组件表；只有长文本使用 draw_control_text。
static UiRect component_absolute_rect(UiRect local) {
    int64_t origin[2]{};
    panel_abs_origin({0, 0, 0, 0}, origin);
    local.x += origin[0];
    local.y += origin[1];
    return local;
}

static void component_text(const char* text, int x, int y, uint32_t color, int align = 0,
                           int font = 1) {
    if (text == nullptr || text[0] == 0 || g_ui_components == nullptr || g_root == nullptr ||
        g_ui_components->draw_control_text == nullptr) return;
    (void)g_ui_components->draw_control_text(g_root, text, x, y, color, align, font);
}

static void component_container(UiRect rect) {
    if (g_ui_components == nullptr || g_ui_components->draw_container == nullptr) return;
    (void)g_ui_components->draw_container(component_absolute_rect(rect));
}

void root_decor_draw() {
    if (g_root == nullptr) return;
    UiRect root{};
    if (ui_get_rect(g_root, &root)) {
        const contracts::QolUiAdapter* ui = ui_adapter();
        if (ui != nullptr && ui->draw_image_part != nullptr) {
            (void)ui->draw_image_part(0x4f, 0, static_cast<int32_t>(root.x + ROOT_W / 2 - 300),
                                      static_cast<int32_t>(root.y + ROOT_H / 2), 2, 1);
            (void)ui->draw_image_part(0x50, 0, static_cast<int32_t>(root.x + ROOT_W / 2 + 300),
                                      static_cast<int32_t>(root.y + ROOT_H / 2), 2, 1);
        }
    }
    component_container({0x10, 0x10, ROOT_W - 0x20, ROOT_H - 0x20});
    component_container({OPS_COL_X, OPS_Y0 - 0x08, OPS_COL_W, 3 * OPS_BTN_H + 2 * OPS_BTN_GAP + 0x10});
    component_container({LIST_COL_X, FRAME_Y, LIST_COL_W, FRAME_H});
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->fill_rect != nullptr) {
        const UiRect root_abs = component_absolute_rect({0, 0, 0, 0});
        constexpr int64_t kSeparatorH = 2;
        for (int i = 0; i < LIST_COLS_PER_PAGE - 1; ++i) {
            const int64_t y = FRAME_Y + FRAME_BORDER + (i + 1) * LIST_ROW_H - 3;
            (void)ui->fill_rect({root_abs.x + LIST_COL_X + FRAME_BORDER, root_abs.y + y,
                                 LIST_COL_W - 2 * FRAME_BORDER, kSeparatorH}, UI_GOLD);
        }
    }
}

void slot_btn_draw(void* ctrl) {
    if (ctrl == nullptr || g_ui_components == nullptr) return;
    int index = -1;
    for (int i = 0; i < 3; ++i) if (ctrl == g_slot_btns[i]) index = i;
    if (index < 0) return;
    const bool selected = g_selected_slot == index;
    component_container(slot_btn_rect(index));
    const UiRect rect = slot_btn_rect(index);
    if (!g_slots[index].exists) {
        char empty[32]; snprintf(empty, sizeof(empty), "存档%d 空", index + 1);
        component_text(empty, static_cast<int>(rect.x + rect.w / 2),
                       static_cast<int>(rect.y + rect.h / 2 - 0x0C),
                       selected ? UI_GOLD : COLOR_TEXT_GRAY, 2, 1);
        return;
    }
    const SlotInfo& info = g_slots[index];
    const char* cls = save_backup_class_name(info.class_idx);
    char line1[64], line2[64];
    snprintf(line1, sizeof(line1), "%s Lv%s", cls != nullptr ? cls : "职业?",
             info.hero_level >= 0 ? std::to_string(info.hero_level).c_str() : "?");
    const std::string map = save_backup_map_name(info.map_id);
    snprintf(line2, sizeof(line2), "%s", map.empty() ? "未知地图" : map.c_str());
    component_text(line1, static_cast<int>(rect.x + rect.w / 2), static_cast<int>(rect.y + 0x18),
                   selected ? UI_GOLD : UI_OPTION_TEXT, 2, 1);
    component_text(line2, static_cast<int>(rect.x + rect.w / 2), static_cast<int>(rect.y + 0x34),
                   selected ? COLOR_TEXT_WHITE : UI_OPTION_TEXT, 2, 1);
}

void ops_btn_draw(void* ctrl) {
    if (ctrl == nullptr || g_ui_components == nullptr) return;
    int index = -1;
    for (int i = 0; i < 3; ++i) if (ctrl == g_ops_btns[i]) index = i;
    if (index < 0) return;
    const bool slot_selected = g_selected_slot >= 0;
    const bool slot_exists = slot_selected && g_slots[g_selected_slot].exists;
    const bool backup_selected = g_selected_auto ? g_auto_backup_valid : selected_manual_backup() != nullptr;
    const bool enabled = index == 0 ? slot_exists :
                         index == 1 ? ((slot_selected != (selected_manual_backup() != nullptr)) && !g_selected_auto) :
                                      (slot_selected && backup_selected);
    if (index != 1) {
        (void)g_ui_components->arrow_button_set_enabled(ctrl,
            enabled && g_dialog == DialogKind::kNone ? 1 : 0);
        (void)g_ui_components->arrow_button_draw(ctrl);
        return;
    }
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui == nullptr || ui->fill_rect == nullptr) return;
    const UiRect rect = component_absolute_rect(ops_btn_rect(index));
    const int64_t x0 = rect.x + (rect.w - 0x20) / 2;
    const int64_t y0 = rect.y + (rect.h - 0x20) / 2;
    const uint32_t color = enabled && g_dialog == DialogKind::kNone ? UI_GOLD : COLOR_TEXT_GRAY;
    for (int i = 0; i < 8; ++i) {
        const int64_t step = i * 4;
        (void)ui->fill_rect({x0 + step, y0 + step, 6, 4}, color);
        (void)ui->fill_rect({x0 + 0x1C - step, y0 + step, 6, 4}, color);
    }
}

void list_btn_draw(void* ctrl) {
    if (ctrl == nullptr || g_ui_components == nullptr) return;
    int local = -1;
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) if (ctrl == g_list_btns[i]) local = i;
    if (local < 0) return;
    const int absolute = g_page_index * LIST_COLS_PER_PAGE + local;
    const BackupEntry* item = backup_item_at_page(local);
    if (item == nullptr) return;
    const bool parsed = item->parsed;
    const bool selected = absolute == g_selected_backup;
    const UiRect rect = list_btn_rect(local);
    if (!parsed) {
        component_text("加载中…", static_cast<int>(rect.x + 0x50), static_cast<int>(rect.y + 0x12), COLOR_TEXT_GRAY);
        return;
    }
    char first[CB_TEXT_SIZE], second[CB_TEXT_SIZE], prefix[CB_TEXT_SIZE], map[48];
    const char* cls = !item->class_name.empty() ? item->class_name.c_str() : save_backup_class_name(item->class_idx);
    snprintf(prefix, sizeof(prefix), "%s", item->hero_level >= 0 ? "" : "Lv? ");
    if (item->hero_level >= 0 && cls != nullptr) snprintf(prefix, sizeof(prefix), "%s Lv%d ", cls, item->hero_level);
    else if (item->hero_level >= 0) snprintf(prefix, sizeof(prefix), "Lv%d ", item->hero_level);
    else if (cls != nullptr) snprintf(prefix, sizeof(prefix), "%s ", cls);
    const std::string map_name = item->map_name.empty() ? "地图" + std::to_string(item->map_id) : item->map_name;
    snprintf(map, sizeof(map), "%s", map_name.c_str());
    snprintf(first, sizeof(first), "%s%s", prefix, map);
    time_t sec = static_cast<time_t>(item->export_time_ms / 1000); tm value{}; localtime_r(&sec, &value);
    char time_text[24]; strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M", &value);
    snprintf(second, sizeof(second), "%s  #%.6s", time_text, item->checksum.c_str());
    char number[8]; snprintf(number, sizeof(number), "%d", absolute + 1);
    const uint32_t primary = selected ? UI_GOLD : UI_OPTION_TEXT;
    component_text(number, static_cast<int>(rect.x + 0x20), static_cast<int>(rect.y + 0x18), primary, 2, 1);
    component_text(first, static_cast<int>(rect.x + 0x48), static_cast<int>(rect.y + 0x07), primary, 0, 1);
    component_text(second, static_cast<int>(rect.x + 0x48), static_cast<int>(rect.y + 0x23), COLOR_TEXT_WHITE, 0, 1);
}

void auto_btn_draw(void* ctrl) {
    if (ctrl == nullptr || g_ui_components == nullptr) return;
    const UiRect rect = auto_btn_rect();
    component_container(rect);
    component_text("auto", static_cast<int>(rect.x + 0x0C), static_cast<int>(rect.y + 0x1F),
                   g_selected_auto ? UI_GOLD : UI_OPTION_TEXT);
    if (!g_auto_backup_valid) {
        component_text(g_selected_slot < 0 ? "未选择存档槽" : "暂无自动备份",
                       static_cast<int>(rect.x + 0x60), static_cast<int>(rect.y + 0x1F), COLOR_TEXT_GRAY);
        return;
    }
    const BackupEntry& e = g_auto_backup;
    const char* cls = !e.class_name.empty() ? e.class_name.c_str() : save_backup_class_name(e.class_idx);
    char line1[CB_TEXT_SIZE], line2[CB_TEXT_SIZE];
    snprintf(line1, sizeof(line1), "%s Lv%d  %s", cls != nullptr ? cls : "职业?", e.hero_level,
             e.map_name.empty() ? "未知地图" : e.map_name.c_str());
    time_t sec = static_cast<time_t>(e.export_time_ms / 1000); tm value{}; localtime_r(&sec, &value);
    char time_text[32]; strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M", &value);
    snprintf(line2, sizeof(line2), "%s  #%.6s", time_text, e.checksum.c_str());
    component_text(line1, static_cast<int>(rect.x + 0x60), static_cast<int>(rect.y + 0x12),
                   g_selected_auto ? UI_GOLD : UI_OPTION_TEXT);
    component_text(line2, static_cast<int>(rect.x + 0x60), static_cast<int>(rect.y + 0x2E), COLOR_TEXT_WHITE);
}

void page_btn_draw(void* ctrl) {
    if (ctrl == nullptr || g_ui_components == nullptr) return;
    const bool next = ctrl == g_next_btn;
    const bool enabled = next ? g_page_index < g_total_pages - 1 : g_page_index > 0;
    (void)g_ui_components->arrow_button_set_enabled(ctrl, enabled ? 1 : 0);
    (void)g_ui_components->arrow_button_draw(ctrl);
    if (!next) {
        char page[24]; snprintf(page, sizeof(page), "%d/%d", g_page_index + 1, g_total_pages > 0 ? g_total_pages : 1);
        component_text(page, static_cast<int>(LIST_COL_X + LIST_COL_W / 2), static_cast<int>(PAGE_ROW_Y + 0x0C), UI_OPTION_TEXT, 2, 1);
    }
}

void msg_label_draw(void*) {
    if (!g_msg_text.empty()) component_text(g_msg_text.c_str(), ROOT_W / 2, MSG_Y + 0x18, g_msg_color, 2, 1);
}

void sync_component_enabled_locked() {
    if (g_ui_components == nullptr) return;
    const bool modal = g_dialog != DialogKind::kNone;
    (void)g_ui_components->nav_button_set_enabled(g_back_btn, modal ? 0 : 1);
    const bool export_enabled = g_selected_slot >= 0 && g_slots[g_selected_slot].exists;
    const bool manual_selected = selected_manual_backup() != nullptr;
    const bool import_enabled = g_selected_slot >= 0 && (g_selected_auto ? g_auto_backup_valid : manual_selected);
    (void)g_ui_components->arrow_button_set_enabled(g_ops_btns[0], !modal && export_enabled ? 1 : 0);
    (void)g_ui_components->arrow_button_set_enabled(g_ops_btns[2], !modal && import_enabled ? 1 : 0);
    (void)g_ui_components->arrow_button_set_enabled(g_prev_btn, !modal && g_page_index > 0 ? 1 : 0);
    (void)g_ui_components->arrow_button_set_enabled(g_next_btn, !modal && g_page_index < g_total_pages - 1 ? 1 : 0);
}
