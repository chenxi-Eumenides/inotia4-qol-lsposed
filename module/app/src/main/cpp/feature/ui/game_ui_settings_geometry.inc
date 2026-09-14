UiRect settings_root_rect() {
    const int64_t screen_w = ROOT_W + static_cast<int64_t>(fn_calc_res_width()) * 2;
    const int64_t screen_h = ROOT_H + static_cast<int64_t>(fn_calc_res_height()) * 2;
    if (screen_w <= 0 || screen_h <= 0) return {0, 0, ROOT_W, ROOT_H};
    const int64_t logical_w = screen_w * ROOT_H / screen_h;
    return {(logical_w - ROOT_W) / 2, 0, ROOT_W, ROOT_H};
}

int settings_collect_logical_pages(int* pages) {
    if (pages == nullptr) return 0;
    int count = 0;
    for (int i = 0; i < kSettingsItemCount; ++i) {
        const int logical_page = kSettingsItems[i].page;
        if (logical_page <= 0) continue;
        int pos = 0;
        while (pos < count && pages[pos] < logical_page) ++pos;
        if (pos < count && pages[pos] == logical_page) continue;
        for (int j = count; j > pos; --j) pages[j] = pages[j - 1];
        pages[pos] = logical_page;
        ++count;
    }
    return count;
}

int settings_page_count() {
    int pages[kSettingsItemCount] = {};
    return settings_collect_logical_pages(pages);
}

int settings_logical_page_at(int physical_page) {
    int pages[kSettingsItemCount] = {};
    const int page_count = settings_collect_logical_pages(pages);
    if (physical_page < 0 || physical_page >= page_count) return -1;
    return pages[physical_page];
}

int settings_item_physical_page(int index) {
    if (index < 0 || index >= kSettingsItemCount) return -1;
    const int logical_page = kSettingsItems[index].page;
    if (logical_page <= 0) return -1;
    int pages[kSettingsItemCount] = {};
    const int page_count = settings_collect_logical_pages(pages);
    for (int i = 0; i < page_count; ++i) {
        if (pages[i] == logical_page) return i;
    }
    return -1;
}

bool settings_item_is_savebackup(int index) {
    return index >= 0 && index < kSettingsItemCount &&
           kSettingsItems[index].key != nullptr && kSettingsItems[index].key[0] == 0;
}

int settings_item_index_at_cell(int cell) {
    if (cell < 0 || cell >= SETTINGS_PAGE_CAPACITY) return -1;
    const int logical_page = settings_logical_page_at(g_page_index);
    if (logical_page < 0) return -1;
    int local_cell = 0;
    for (int i = 0; i < kSettingsItemCount; ++i) {
        if (kSettingsItems[i].page != logical_page) continue;
        if (local_cell == cell) return i;
        ++local_cell;
    }
    return -1;
}

int settings_item_cell_at_current_page(int index) {
    if (index < 0 || index >= kSettingsItemCount) return -1;
    if (settings_item_physical_page(index) != g_page_index) return -1;
    const int logical_page = settings_logical_page_at(g_page_index);
    if (logical_page < 0) return -1;
    int cell = 0;
    for (int i = 0; i < index; ++i) {
        if (kSettingsItems[i].page == logical_page) ++cell;
    }
    return cell < SETTINGS_PAGE_CAPACITY ? cell : -1;
}

void settings_sync_page_state() {
    g_total_pages = settings_page_count();
    if (g_total_pages <= 0) {
        g_total_pages = 1;
        g_page_index = 0;
    } else if (g_page_index >= g_total_pages) {
        g_page_index = g_total_pages - 1;
    } else if (g_page_index < 0) {
        g_page_index = 0;
    }
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
