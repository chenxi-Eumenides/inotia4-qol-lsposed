UiRect settings_root_rect() {
    const int64_t screen_w = ROOT_W + static_cast<int64_t>(fn_calc_res_width()) * 2;
    const int64_t screen_h = ROOT_H + static_cast<int64_t>(fn_calc_res_height()) * 2;
    if (screen_w <= 0 || screen_h <= 0) return {0, 0, ROOT_W, ROOT_H};
    const int64_t logical_w = screen_w * ROOT_H / screen_h;
    return {(logical_w - ROOT_W) / 2, 0, ROOT_W, ROOT_H};
}

UiRect settings_row_rect(int index) {
    const int column = index % SETTINGS_COLUMN_COUNT;
    const int row = index / SETTINGS_COLUMN_COUNT;
    return {CONTENT_X + column * CELL_W, GRID_Y + row * CELL_H, CELL_W, CELL_H};
}

UiRect settings_label_rect(int index) {
    UiRect cell = settings_row_rect(index);
    constexpr int64_t label_h = 0x28;
    return {cell.x + CELL_PADDING, cell.y + (cell.h - label_h) / 2,
            cell.w - ROW_BTN_W - CELL_PADDING * 3, label_h};
}

UiRect settings_toggle_rect(int index) {
    UiRect cell = settings_row_rect(index);
    return {cell.x + cell.w - ROW_BTN_W - CELL_PADDING,
            cell.y + (cell.h - ROW_BTN_H) / 2, ROW_BTN_W, ROW_BTN_H};
}

// 存档备份按钮（v0.7.x）：底部独立条，点击后 push 打开 savebackup 面板。
UiRect settings_savebackup_rect() {
    return {CONTENT_X, GRID_Y + VISIBLE_GRID_ROWS * CELL_H + 0x08,
            CONTENT_W, ROW_BTN_H + 0x08};
}

// ---- Kotlin 配置桥接（native 反调 ModuleConfigUiBridge）----

