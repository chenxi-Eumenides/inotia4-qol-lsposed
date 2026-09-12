// 控件 DrawProc：自绘边框 + 文本 + 选中高亮
//
// 颜色为 ABGR（与扩展背包一致；游戏 fn_grpx_fill_rect 接受 ABGR）。

void root_decor_draw() {
    if (g_root == nullptr) return;
    int64_t pt[2];
    ctrl_abs_point(g_root, pt);
    // 半透明遮罩（屏幕宽 = ROOT_W + 边距×2）
    if (fn_grpx_fill_rect_alpha != nullptr && fn_calc_res_width != nullptr) {
        const int screen_w = static_cast<int>(ROOT_W + static_cast<int64_t>(fn_calc_res_width()) * 2);
        fn_grpx_fill_rect_alpha(0, 0, screen_w, static_cast<int>(ROOT_H), 0xFF000000, 0x3c);
    }
    // 面板底色 + 描边
    if (fn_grpx_fill_rect != nullptr) {
        // 面板：宽 CONTENT_W × ROOT_H，居中
        const int panel_x = static_cast<int>(pt[0]) + static_cast<int>(CONTENT_X) - 0x04;
        const int panel_y = static_cast<int>(pt[1]) + 0x04;
        const int panel_w = static_cast<int>(CONTENT_W) + 0x08;
        const int panel_h = static_cast<int>(ROOT_H) - 0x08;
        fn_grpx_fill_rect(panel_x, panel_y, panel_w, panel_h, COLOR_PANEL);
        // 描边
        fn_grpx_fill_rect(panel_x, panel_y, panel_w, 2, COLOR_BORDER);
        fn_grpx_fill_rect(panel_x, panel_y + panel_h - 2, panel_w, 2, COLOR_BORDER);
        fn_grpx_fill_rect(panel_x, panel_y, 2, panel_h, COLOR_BORDER);
        fn_grpx_fill_rect(panel_x + panel_w - 2, panel_y, 2, panel_h, COLOR_BORDER);
        // 标题条
        fn_grpx_fill_rect(panel_x, panel_y, panel_w, 0x24, 0xFF0A0608);
        // 三栏分隔竖线（左|中 = OPS_COL_X；中|右 = LIST_COL_X）
        const int col1_x = static_cast<int>(pt[0]) + static_cast<int>(OPS_COL_X);
        fn_grpx_fill_rect(col1_x, static_cast<int>(pt[1]) + static_cast<int>(CONTENT_Y),
                          2, static_cast<int>(CONTENT_H), COLOR_BORDER);
        const int col2_x = static_cast<int>(pt[0]) + static_cast<int>(LIST_COL_X);
        fn_grpx_fill_rect(col2_x, static_cast<int>(pt[1]) + static_cast<int>(CONTENT_Y),
                          2, static_cast<int>(CONTENT_H), COLOR_BORDER);
        // 标题文本（居中）：与 game_ui_kit ui_draw_text_centered 同路径。
        // 注意：fn_grpx_set_font_color（非 RGB 版）在本版本不作用于文字渲染，
        // 设色必须走 fn_grpx_set_font_color_rgb（ABGR 拆包），见 ui_draw_text。
        if (fn_ui_draw_string_halign != nullptr) {
            if (fn_grpx_set_font_color_rgb != nullptr) {
                fn_grpx_set_font_color_rgb(static_cast<int32_t>(COLOR_TEXT & 0xff),
                                           static_cast<int32_t>((COLOR_TEXT >> 8) & 0xff),
                                           static_cast<int32_t>((COLOR_TEXT >> 16) & 0xff));
            }
            fn_ui_draw_string_halign(const_cast<char*>(kTitle),
                                     panel_x + panel_w / 2,
                                     static_cast<int>(pt[1]) + 0x0C, 1, 1);
        }
    }
}

// 文本绘制：与 game_ui_kit ui_draw_text 同路径——
// fn_grpx_set_font_color_rgb(ABGR 拆包) + fn_grpx_draw_string_with_font(text, x, y, align=0, font=1)。
// 旧路径 fn_grpx_set_font_color(color) + fn_ui_draw_string_halign(..., 1, 0) 在真机上不出字：
// set_font_color 非 RGB 版不作用于文字，且 halign 第 4 参是 font、第 5 参才是 align（kit 签名）。
static void draw_string_at(int x, int y, const char* s, uint32_t color) {
    if (s == nullptr || s[0] == 0) return;
    if (fn_grpx_set_font_color_rgb == nullptr || fn_grpx_draw_string_with_font == nullptr) return;
    fn_grpx_set_font_color_rgb(static_cast<int32_t>(color & 0xff),
                               static_cast<int32_t>((color >> 8) & 0xff),
                               static_cast<int32_t>((color >> 16) & 0xff));
    fn_grpx_draw_string_with_font(const_cast<char*>(s), x, y, 0, 1);
}

static void draw_two_line_text(int x, int y, const char* line1, const char* line2,
                               uint32_t c1, uint32_t c2) {
    draw_string_at(x, y, line1, c1);
    draw_string_at(x, y + 0x14, line2, c2);
}

void slot_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int idx = -1;
    for (int i = 0; i < 3; ++i) if (ctrl == g_slot_btns[i]) { idx = i; break; }
    if (idx < 0) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const UiRect r = slot_btn_rect(idx);
    if (fn_grpx_fill_rect != nullptr) {
        const bool selected = (idx == g_selected_slot);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), static_cast<int>(r.h),
                          selected ? COLOR_HIGHLIGHT_SEL : COLOR_HIGHLIGHT);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1] + r.h - 2),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        if (selected) {
            // 左侧条带强调
            fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                              4, static_cast<int>(r.h), COLOR_BORDER);
        }
    }
    // 文案：「槽N Lv.X」或「槽N (空)」——只读后台刷新缓存的 g_slots（read_slots_into
    // 按 data_save_slots_json / read_hero_meta 同口径填充），DrawProc 不再直接读游戏内存。
    char buf[CB_TEXT_SIZE];
    const SlotInfo& info = g_slots[idx];
    if (info.exists && info.hero_level >= 0) {
        snprintf(buf, sizeof(buf), "槽%d Lv%d", idx, info.hero_level);
    } else if (info.exists) {
        snprintf(buf, sizeof(buf), "槽%d", idx);
    } else {
        snprintf(buf, sizeof(buf), "槽%d (空)", idx);
    }
    draw_string_at(static_cast<int>(pt[0]) + 0x0A, static_cast<int>(pt[1]) + 0x0E,
                   buf, COLOR_TEXT);
}

void ops_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int idx = -1;
    for (int i = 0; i < 3; ++i) if (ctrl == g_ops_btns[i]) { idx = i; break; }
    if (idx < 0) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const UiRect r = ops_btn_rect(idx);
    static const char* kLabels[3] = {"→ 导出", "← 导入", "删除"};
    if (fn_grpx_fill_rect != nullptr) {
        const bool disabled =
            (idx == 0 && g_backups.empty()) ||  // 导出始终可用（可能产生新备份）
            (idx == 1 && g_selected_backup < 0) ||
            (idx == 2 && g_selected_backup < 0);
        const uint32_t bg = disabled ? 0xFF181012 : COLOR_HIGHLIGHT;
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), static_cast<int>(r.h), bg);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1] + r.h - 2),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
    }
    // 中栏较窄（OPS_BTN_W=0x50）：单行标签（含箭头）即可；两行 hint 会与标签重叠并横向溢出。
    draw_string_at(static_cast<int>(pt[0]) + 0x06, static_cast<int>(pt[1]) + 0x16,
                   kLabels[idx], COLOR_TEXT);
}

void list_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int local = -1;
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
        if (ctrl == g_list_btns[i]) { local = i; break; }
    }
    if (local < 0) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const UiRect r = list_btn_rect(local);
    const BackupEntry* e = backup_at_page(local);
    if (fn_grpx_fill_rect != nullptr) {
        const bool selected = (local == g_selected_backup);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), static_cast<int>(r.h),
                          e == nullptr ? 0xFF181012 :
                              (selected ? COLOR_HIGHLIGHT_SEL : COLOR_HIGHLIGHT));
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1] + r.h - 2),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        if (selected) {
            fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                              4, static_cast<int>(r.h), COLOR_BORDER);
        }
    }
    if (e == nullptr) {
        // 后台刷新进行中且当前无数据：显示加载中而非误导性的「(空)」。
        const bool loading = g_backups_loading.load() && g_backups.empty();
        draw_string_at(static_cast<int>(pt[0]) + 0x08, static_cast<int>(pt[1]) + 0x14,
                       local == 0 && loading ? "加载中…" : "(空)", COLOR_TEXT_DIM);
        return;
    }
    // 文案直接取面板缓存的 g_backups 条目（backup_at_page = g_page_index*4+local，与事件
    // 命中/操作回调用同一 g_page_index 换算，口径一致）。绘制路径零 IO：
    // 此前每帧调 save_backup_list_json()（目录遍历 + 逐 bundle 读文件解密 + 现场解析）。
    char line1[CB_TEXT_SIZE];
    char line2[CB_TEXT_SIZE];
    // map_name 为 UTF-8 中文（最长约 32 字节）；截断到 16 字节防止溢出列表项宽度。
    char map_short[32];
    if (!e->map_name.empty()) {
        size_t len = e->map_name.size();
        if (len > 16) len = 16;
        memcpy(map_short, e->map_name.c_str(), len);
        map_short[len] = 0;
    } else {
        snprintf(map_short, sizeof(map_short), "地图%d", e->map_id);
    }
    // 第一行：Lv.X 地图名
    snprintf(line1, sizeof(line1), "Lv%d  %s",
             e->hero_level >= 0 ? e->hero_level : -1, map_short);
    // 第二行：备份时间 + 校验前 6 位
    time_t et_sec = static_cast<time_t>(e->export_time_ms / 1000);
    tm tm_et{};
    localtime_r(&et_sec, &tm_et);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "%m-%d %H:%M", &tm_et);
    char checksum_short[8];
    size_t cklen = e->checksum.size() < 6 ? e->checksum.size() : 6;
    memcpy(checksum_short, e->checksum.c_str(), cklen);
    checksum_short[cklen] = 0;
    snprintf(line2, sizeof(line2), "%s  #%s", time_buf, checksum_short);
    draw_two_line_text(static_cast<int>(pt[0]) + 0x08, static_cast<int>(pt[1]) + 0x04,
                       line1, line2, COLOR_TEXT, COLOR_TEXT_DIM);
}

void page_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const bool is_next = (ctrl == g_next_btn);
    const UiRect r = page_btn_rect(is_next);
    const bool disabled = is_next ? (g_page_index >= g_total_pages - 1) : (g_page_index <= 0);
    if (fn_grpx_fill_rect != nullptr) {
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), static_cast<int>(r.h),
                          disabled ? 0xFF181012 : COLOR_HIGHLIGHT);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1] + r.h - 2),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
    }
    char label[CB_TEXT_SIZE];
    if (is_next) {
        snprintf(label, sizeof(label), "下一页 %d/%d", g_page_index + 1,
                 g_total_pages > 0 ? g_total_pages : 1);
    } else {
        snprintf(label, sizeof(label), "上一页");
    }
    draw_string_at(static_cast<int>(pt[0]) + 0x06, static_cast<int>(pt[1]) + 0x10,
                   label, disabled ? COLOR_TEXT_DIM : COLOR_TEXT);
}

void back_btn_draw(void* ctrl) {
    if (ctrl == nullptr || ctrl != g_back_btn) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const UiRect r = back_btn_rect();
    if (fn_grpx_fill_rect != nullptr) {
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), static_cast<int>(r.h), COLOR_HIGHLIGHT);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1] + r.h - 2),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
    }
    // 按钮高 HINT_H-0x08=0x30：单行标签，y 偏移与 slot_btn(0x0E) 同款居中口径。
    draw_string_at(static_cast<int>(pt[0]) + 0x0A, static_cast<int>(pt[1]) + 0x0E,
                   "← 返回", COLOR_TEXT);
}

void msg_label_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const UiRect r = msg_label_rect();
    if (fn_grpx_fill_rect != nullptr) {
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), static_cast<int>(r.h), 0xFF0A0608);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1] + r.h - 2),
                          static_cast<int>(r.w), 2, COLOR_BORDER);
        fn_grpx_fill_rect(static_cast<int>(pt[0]), static_cast<int>(pt[1]),
                          2, static_cast<int>(r.h), COLOR_BORDER);
        fn_grpx_fill_rect(static_cast<int>(pt[0] + r.w - 2), static_cast<int>(pt[1]),
                          2, static_cast<int>(r.h), COLOR_BORDER);
    }
    if (g_msg_text.empty()) {
        draw_string_at(static_cast<int>(pt[0]) + 0x06, static_cast<int>(pt[1]) + 0x10,
                       "等待操作…", COLOR_TEXT_DIM);
    } else {
        // 长消息截断到 28 字节（防止溢出）
        char buf[64];
        size_t len = g_msg_text.size();
        if (len > 56) len = 56;
        memcpy(buf, g_msg_text.c_str(), len);
        buf[len] = 0;
        draw_string_at(static_cast<int>(pt[0]) + 0x06, static_cast<int>(pt[1]) + 0x10,
                       buf, g_msg_color);
    }
}
