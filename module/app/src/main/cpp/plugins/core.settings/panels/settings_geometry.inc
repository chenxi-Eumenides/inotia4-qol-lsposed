UiRect settings_root_rect() {
    int32_t inset_w = 0;
    int32_t inset_h = 0;
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui == nullptr || ui->screen_inset_width == nullptr || ui->screen_inset_height == nullptr ||
        !contracts::succeeded(ui->screen_inset_width(&inset_w)) ||
        !contracts::succeeded(ui->screen_inset_height(&inset_h))) {
        return {0, 0, ROOT_W, ROOT_H};
    }
    const int64_t screen_w = ROOT_W + static_cast<int64_t>(inset_w) * 2;
    const int64_t screen_h = ROOT_H + static_cast<int64_t>(inset_h) * 2;
    if (screen_w <= 0 || screen_h <= 0) return {0, 0, ROOT_W, ROOT_H};
    const int64_t logical_w = screen_w * ROOT_H / screen_h;
    return {(logical_w - ROOT_W) / 2, 0, ROOT_W, ROOT_H};
}

// 分页规则唯一实现在 panels/rules.h（host 可测）；本文件只做表/状态薄封装。
int settings_collect_logical_pages(int* pages) {
    return panels_rules::collect_logical_pages(kSettingsItems, kSettingsItemCount, pages,
                                               kSettingsItemCount);
}

int settings_page_count() {
    return panels_rules::page_count(kSettingsItems, kSettingsItemCount);
}

int settings_logical_page_at(int physical_page) {
    return panels_rules::logical_page_at(kSettingsItems, kSettingsItemCount, physical_page);
}

int settings_item_physical_page(int index) {
    return panels_rules::item_physical_page(kSettingsItems, kSettingsItemCount, index);
}

bool settings_item_is_savebackup(int index) {
    return panels_rules::item_is_action(kSettingsItems, kSettingsItemCount, index);
}

int settings_item_index_at_cell(int cell) {
    return panels_rules::item_index_at_cell(kSettingsItems, kSettingsItemCount, g_page_index, cell);
}

int settings_item_cell_at_current_page(int index) {
    return panels_rules::item_cell_at_page(kSettingsItems, kSettingsItemCount, index, g_page_index);
}

void settings_sync_page_state() {
    panels_rules::sync_page_state(kSettingsItems, kSettingsItemCount, &g_page_index, &g_total_pages);
}

UiRect settings_row_rect(int cell) {
    const int column = cell % SETTINGS_COLUMN_COUNT;
    const int row = cell / SETTINGS_COLUMN_COUNT;
    return {CONTENT_X + column * CELL_W, GRID_Y + row * CELL_H, CELL_W, CELL_H};
}

UiRect settings_label_rect(int cell) {
    UiRect cell_rect = settings_row_rect(cell);
    constexpr int64_t label_h = 0x28;
    return {cell_rect.x + CELL_PADDING, cell_rect.y + (cell_rect.h - label_h) / 2,
            cell_rect.w - ROW_BTN_W - CELL_PADDING * 3, label_h};
}

UiRect settings_toggle_rect(int cell) {
    UiRect cell_rect = settings_row_rect(cell);
    return {cell_rect.x + cell_rect.w - ROW_BTN_W - CELL_PADDING,
            cell_rect.y + (cell_rect.h - ROW_BTN_H) / 2, ROW_BTN_W, ROW_BTN_H};
}

UiRect settings_savebackup_rect(int cell) {
    UiRect toggle = settings_toggle_rect(cell);
    return {toggle.x + toggle.w - SAVEBACKUP_BTN_W, toggle.y,
            SAVEBACKUP_BTN_W, toggle.h};
}

UiRect settings_page_button_rect(bool next) {
    // 原始网格宽度几乎占满横向空间；2×4 后利用网格下方空间排布左右箭头，
    // 与页码形成一组导航，不侵入任何网格单元。
    return {next ? ROOT_W / 2 + PAGE_NAV_TEXT_GAP
                 : ROOT_W / 2 - PAGE_NAV_TEXT_GAP - PAGE_BUTTON_W,
            PAGE_NAV_Y, PAGE_BUTTON_W, PAGE_BUTTON_H};
}

UiRect settings_page_indicator_rect() {
    return {0, PAGE_INDICATOR_Y, ROOT_W, PAGE_INDICATOR_H};
}

void settings_sync_row_rects() {
    settings_sync_page_state();
    for (int i = 0; i < kSettingsItemCount; ++i) {
        const int cell = settings_item_cell_at_current_page(i);
        if (g_rows[i].btn != nullptr) {
            const UiRect rect = cell >= 0 ?
                (settings_item_is_savebackup(i) ? settings_savebackup_rect(cell)
                                                : settings_toggle_rect(cell))
                : UiRect{0, 0, 0, 0};
            ui_set_rect(g_rows[i].btn, rect);
        }
        if (g_rows[i].desc != nullptr) {
            ui_set_rect(g_rows[i].desc, cell >= 0 ? settings_label_rect(cell) : UiRect{0, 0, 0, 0});
        }
    }
}

// ---- Kotlin 配置桥接（native 反调 ModuleConfigUiBridge）----
