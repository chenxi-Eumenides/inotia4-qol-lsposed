// 控件 DrawProc：与 game_ui_settings 同一套视觉语言
//
// - 背景：title background 贴图左右两半 + 半透明遮罩（root_decor_draw，同
//   settings_panel_process 的背景/遮罩口径），不再自绘面板底色/边框矩形。
// - 分区：不做整体大框。左栏存档区、右栏备份列表区各一个 UI_GOLD 3px 框
//   （ui_draw_panel_decor 上下边 + ui_draw_vertical_line 左右边，同设置页粗细/颜色）；
//   左右两栏同宽（COL_W=0x192），高度按行阵精确计算（左 3 槽 SLOT_FRAME_H，右 5 行 FRAME_H）；
//   三栏沿公共水平中线 CONTENT_MID_Y 垂直居中（左框顶 SLOT_FRAME_Y、中栏顶 OPS_Y0）；
//   中栏操作按钮不组框，每个按钮 ui_custom::draw_button 自带 2px 边框。
// - 行分割线：落在行间 4px 空隙内（pitch - 按钮高 = 4），3px 线不压任何按钮。
// - 选中态：深琥珀填充（COLOR_SEL_BG=0x5182 RGB565 → RGB(0x50,0x30,0x10) + alpha 0x50=80%，
//   ui_fill_rect_alpha/GRPX_FillRectAlpha 口径），暗底反衬金色文字；
//   不画边框；适用左栏选中槽与右栏选中备份项。再次点击已选项取消选中（-1）。
// - 按钮：操作/翻页/返回 fallback 走 ui_custom::draw_button（设置页 fallback 按钮样式）。
//   中栏操作按钮文字随可用性：可用 UI_GOLD / 不可用 COLOR_TEXT_GRAY（灰态仍可点击给提示）。
// - 文本：fn_grpx_set_font_color_rgb(ABGR 拆包) + fn_grpx_draw_string_with_font(text, x, y,
//   align, font)。align 语义（libgame 0x8f9b0→0x9b12c→0x96adc 反汇编：align==1 → 画位
//   x-width=右对齐、align==2 → x-width/2=居中、其余左对齐；halign 链 0xa72ec 同结论）：
//   0=左 / 1=右 / 2=中（game_symbols.h 旧注释「0左/1中/2右」有误，勿参考）。
//   居中文本一律 align=2；左对齐正文 align=0。
//   font 为字体表下标：GRPX_GetFontSize(0x8fc14) 对 0..3 返回 {20,24,38,28}；左/右两栏正文
//   统一 font=3(SB_TEXT_FONT, 28/行高30)，放大后按估算宽度截断地图名（见 sb_* 助手）。
//   旧路径 fn_grpx_set_font_color(color) + fn_ui_draw_string_halign(..., 1, 0) 在真机上不出字：
//   set_font_color 非 RGB 版不作用于文字，且 halign 第 4 参是 font、第 5 参才是 align；
//   ui_draw_text_centered 内部走 halign，同样不出字。
// - 配色：左栏空槽 COLOR_TEXT_GRAY / 有档槽 UI_GOLD；右栏备份项首行 UI_GOLD、
//   次行 COLOR_TEXT_WHITE；正文 UI_OPTION_TEXT、次要/提示 UI_GOLD。
//   左栏/右栏首行文案：<职业名> Lv<L> <地图名>（职业→等级→地图）；旧备份无职业字段时退化。
//
// 颜色均为 ABGR（与 game_ui_settings 同值：UI_GOLD=0xFF00B4D7，UI_OPTION_TEXT=0xFFCB9EE2），
// 唯一例外是选中底色（ui_fill_rect_alpha 用 RGB565+百分比 alpha，见上）。

// 画一个 3px 分区框：上下边线（含行分割线）走 ui_draw_panel_decor，左右边线走
// ui_draw_vertical_line；框内透明。角部 3×3 像素由相邻边线各画一次，视觉为完整框。
static void draw_frame_box(const UiRect& frame, const int64_t* seps, size_t sep_count) {
    ui_draw_panel_decor(frame, seps, sep_count, UI_GOLD);
    ui_draw_vertical_line(frame.x, frame.y, frame.h, UI_GOLD, 3);
    ui_draw_vertical_line(frame.x + frame.w - 3, frame.y, frame.h, UI_GOLD, 3);
}

void root_decor_draw() {
    if (g_root == nullptr) return;
    UiRect root_rect{};
    if (!ui_get_rect(g_root, &root_rect)) return;
    // ① 背景图：与设置页同款 title background 左右两半（enter 加载；失败仅提示一次）。
    //    坐标口径 = settings_panel_process 的屏幕坐标（设置页 root.y 恒 0）：
    //    x = root.x + ROOT_W/2 ∓ 0x12c：由 panel_root_rect 的 root.x=(logical_w-ROOT_W)/2
    //        恒等推出 = 屏幕逻辑中心 logical_w/2 ∓ 0x12c，与内容/页码无关；
    //    y = ROOT_H/2 = 0x140 固定（设置页即 root.y(=0)+ROOT_H/2）。本面板现 root.y=0，
    //        与设置页背景在屏幕上逐像素同位。
    if (g_title_background_images_loaded) {
        ui_draw_image_part(TITLE_BACKGROUND_LEFT_UNIT, TITLE_BACKGROUND_LOC,
                           static_cast<int32_t>(root_rect.x + ROOT_W / 2 - 0x12c),
                           static_cast<int32_t>(ROOT_H / 2), 2, 1);
        ui_draw_image_part(TITLE_BACKGROUND_RIGHT_UNIT, TITLE_BACKGROUND_LOC,
                           static_cast<int32_t>(root_rect.x + ROOT_W / 2 + 0x12c),
                           static_cast<int32_t>(ROOT_H / 2), 2, 1);
    } else if (!g_background_unavailable_logged) {
        QOL_LOG_DEBUG(QolDomain::kUi, "background image unavailable; drawing mask + lines only");
        g_background_unavailable_logged = true;
    }
    // ② 半透明遮罩（同设置页 alpha 0x50）：矩形与 settings_panel_process 完全同口径
    //    {0, 0, ROOT_W+root.x*2, ROOT_H}（屏幕逻辑空间，顶固定 0、高 ROOT_H），
    //    不随 root.y 扩展 —— 与背景图同一固定基准，无上下偏移。
    ui_fill_rect_alpha({0, 0, ROOT_W + root_rect.x * 2, ROOT_H}, 0xFF000000, 0x50);
    if (fn_grpx_fill_rect == nullptr) return;
    // ③ 左栏框（存档区）：与其他栏同宽（COL_W），高度按行阵精确计算（SLOT_FRAME_H=3*ROW_H+2*3-4），
    //    框顶 SLOT_FRAME_Y（三栏公共水平中线对齐，见 cpp 常量区验算）。
    //    行分割线落在行间 4px 空隙内：y = 框内顶 + i*ROW_H - SLOT_ROW_GAP，3px 线占
    //    [y, y+3)，行 i 顶 = 框内顶 + i*ROW_H，不压按钮/选中背景（按钮高 ROW_H-4）。
    const int64_t slot_seps[2] = {
        root_rect.y + SLOT_FRAME_Y + FRAME_BORDER + 1 * SLOT_ROW_H - SLOT_ROW_GAP,
        root_rect.y + SLOT_FRAME_Y + FRAME_BORDER + 2 * SLOT_ROW_H - SLOT_ROW_GAP,
    };
    const UiRect slot_frame = {root_rect.x + SLOT_COL_X, root_rect.y + SLOT_FRAME_Y,
                               SLOT_COL_W, SLOT_FRAME_H};
    draw_frame_box(slot_frame, slot_seps, 2);
    // ④ 右栏框（备份列表区）：分割线随当前页条目数垂直居中分布，与 list_btn_rect 同公式。
    const int n = page_entry_count();
    int64_t list_seps[LIST_COLS_PER_PAGE - 1];
    size_t list_sep_count = 0;
    // 每行下方画一条分割线（最后一行除外）：n≤4 → n 条；n=5 → 4 条（第 5 行下方即框内下缘）。
    // 顶部对齐（首行顶 = 框内上缘），线落在行 i 底与行 i+1 顶之间的 4px 缝隙内：
    // y = 框内顶 + (i+1)*LIST_ROW_H − LIST_ROW_GAP；不压选中背景（按钮高 ROW_H−GAP）。
    const int sep_rows = n < LIST_COLS_PER_PAGE ? n : LIST_COLS_PER_PAGE - 1;
    const int64_t top = FRAME_Y + FRAME_BORDER;
    for (int i = 0; i < sep_rows; ++i) {
        list_seps[list_sep_count++] = root_rect.y + top + (i + 1) * LIST_ROW_H - LIST_ROW_GAP;
    }
    const UiRect list_frame = {root_rect.x + LIST_COL_X, root_rect.y + FRAME_Y,
                               LIST_COL_W, FRAME_H};
    draw_frame_box(list_frame, list_seps, list_sep_count);
    // ⑤ 空列表提示：右框中心（加载中 / 暂无备份）。走 fn_grpx_draw_string_with_font align=2
    //    居中（align 语义见文件头；fn_ui_draw_string_halign 在自定义面板下不出字，勿用）。
    if (n == 0 && fn_grpx_draw_string_with_font != nullptr) {
        const bool loading = g_backups_loading.load() && g_backups.empty();
        if (fn_grpx_set_font_color_rgb != nullptr) {
            fn_grpx_set_font_color_rgb(static_cast<int32_t>(UI_GOLD & 0xff),
                                       static_cast<int32_t>((UI_GOLD >> 8) & 0xff),
                                       static_cast<int32_t>((UI_GOLD >> 16) & 0xff));
        }
        fn_grpx_draw_string_with_font(
            const_cast<char*>(loading ? "加载中…" : "暂无备份"),
            static_cast<int>(root_rect.x + LIST_COL_X + LIST_COL_W / 2),
            static_cast<int>(root_rect.y + FRAME_Y + FRAME_H / 2 - 0x0e), 2, 1);
    }
    // ⑥ 页码指示（如「1/2」）：上一页/下一页两按钮正中间（LIST_COL_X + LIST_COL_W/2，
    //    见 game_ui_savebackup.cpp 翻页按钮注释的间隙验算），只画文字、不画任何框。
    //    align=2 才是真正以 x 为中心（align=1 是右对齐：文字右缘贴 x，页码会整体左偏）。
    //    垂直：按钮高 0x30、文本高约 0x18 → y = PAGE_ROW_Y + (0x30-0x18)/2 = +0x0C（同按钮文本口径）。
    if (fn_grpx_draw_string_with_font != nullptr) {
        char page_buf[16];
        snprintf(page_buf, sizeof(page_buf), "%d/%d", g_page_index + 1,
                 g_total_pages > 0 ? g_total_pages : 1);
        if (fn_grpx_set_font_color_rgb != nullptr) {
            fn_grpx_set_font_color_rgb(static_cast<int32_t>(UI_OPTION_TEXT & 0xff),
                                       static_cast<int32_t>((UI_OPTION_TEXT >> 8) & 0xff),
                                       static_cast<int32_t>((UI_OPTION_TEXT >> 16) & 0xff));
        }
        fn_grpx_draw_string_with_font(
            page_buf,
            static_cast<int>(root_rect.x + LIST_COL_X + LIST_COL_W / 2),
            static_cast<int>(root_rect.y + PAGE_ROW_Y + 0x0C), 2, 1);
    }
    // ⑦ 面板标题已并入顶部居中消息区（msg_label_draw：无消息时显示标题），此处不再绘制。
}

// ============== 文本宽度估算（font=3，逻辑 px）与 UTF-8 截断 ==============
// 依据 libgame 字形循环 0xa72ec / 测宽 0xa48d4：font=1 原生 ASCII=6、空格=4、CJK=9；
// 面板逻辑 = 原生×2；font=3/font=1 字号比 = 28/24（尺寸表），故 font=3 逻辑 ≈
// ASCII=0x0F(15)、空格=0x0A(10)、CJK/其他多字节=0x16(22)，向上取整保守。
// 仅用于渲染前截断，不参与命中测试。
static int sb_glyph_width_font3(unsigned char c) {
    if (c < 0x80) return (c == ' ') ? 0x0A : 0x0F;
    return 0x16;
}
static int sb_est_width_font3(const char* s) {
    if (s == nullptr) return 0;
    int w = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0) {
        const unsigned char c = *p;
        const int n = (c < 0x80) ? 1 : ((c >> 5) == 0x6 ? 2 : ((c >> 4) == 0xE ? 3 : 4));
        w += sb_glyph_width_font3(c);
        for (int k = 0; k < n && *p != 0; ++k) ++p;
    }
    return w;
}
// 把 UTF-8 字符串按码点边界截断到 max_w（font=3 逻辑 px），原地写回（保证合法 UTF-8）。
static void sb_truncate_utf8_font3(char* s, int max_w) {
    if (s == nullptr) return;
    if (max_w < 0) max_w = 0;
    int w = 0;
    unsigned char* p = reinterpret_cast<unsigned char*>(s);
    while (*p != 0) {
        const unsigned char c = *p;
        const int n = (c < 0x80) ? 1 : ((c >> 5) == 0x6 ? 2 : ((c >> 4) == 0xE ? 3 : 4));
        const int gw = sb_glyph_width_font3(c);
        if (w + gw > max_w) break;
        w += gw;
        for (int k = 0; k < n && *p != 0; ++k) ++p;
    }
    *p = 0;
}

static void draw_string_at_font(int x, int y, const char* s, uint32_t color, int font) {
    if (s == nullptr || s[0] == 0) return;
    if (fn_grpx_set_font_color_rgb == nullptr || fn_grpx_draw_string_with_font == nullptr) return;
    fn_grpx_set_font_color_rgb(static_cast<int32_t>(color & 0xff),
                               static_cast<int32_t>((color >> 8) & 0xff),
                               static_cast<int32_t>((color >> 16) & 0xff));
    fn_grpx_draw_string_with_font(const_cast<char*>(s), x, y, 0, font);
}

static void draw_string_at(int x, int y, const char* s, uint32_t color) {
    draw_string_at_font(x, y, s, color, 1);
}

// 两行文本：line2 相对 line1 的 y 偏移 = gap（font=3 口径，见 cpp LIST_LINE_GAP）。
static void draw_two_line_text_font(int x, int y, const char* line1, const char* line2,
                                    uint32_t c1, uint32_t c2, int gap, int font) {
    draw_string_at_font(x, y, line1, c1, font);
    draw_string_at_font(x, y + gap, line2, c2, font);
}

// 水平居中绘制：align=2 时 fn_grpx_draw_string_with_font 以 x 为文字中心
// （align 语义见文件头：0=左/1=右/2=中）。用于按钮文字在框内水平居中；
// 不得用 fn_ui_draw_string_halign / ui_draw_text_centered（自定义面板下不出字）。
static void draw_string_centered_at(int cx, int y, const char* s, uint32_t color) {
    if (s == nullptr || s[0] == 0) return;
    if (fn_grpx_set_font_color_rgb == nullptr || fn_grpx_draw_string_with_font == nullptr) return;
    fn_grpx_set_font_color_rgb(static_cast<int32_t>(color & 0xff),
                               static_cast<int32_t>((color >> 8) & 0xff),
                               static_cast<int32_t>((color >> 16) & 0xff));
    fn_grpx_draw_string_with_font(const_cast<char*>(s), cx, y, 2, 1);
}

void slot_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int idx = -1;
    for (int i = 0; i < 3; ++i) if (ctrl == g_slot_btns[i]) { idx = i; break; }
    if (idx < 0) return;
    const UiRect r = slot_btn_rect(idx);
    // 位置用「root 绝对原点 + 计算 rect」，与命中测试口径一致（不读控件滞后 RECT）。
    int64_t pt[2];
    panel_abs_origin(r, pt);
    // 选中槽：深琥珀暗色填充（ui_fill_rect_alpha，COLOR_SEL_BG(RGB565) + alpha 0x50，无描边）；
    // 暗底反衬金色槽文字，绘制在文本之前，文字浮于其上。
    if (idx == g_selected_slot) {
        ui_fill_rect_alpha({pt[0], pt[1], r.w, r.h}, COLOR_SEL_BG, COLOR_SEL_BG_ALPHA);
    }
    // 文案：只读后台刷新缓存的 g_slots（read_slots_into 在游戏线程填充 exists/hero_level/
    // class_idx/map_id），DrawProc 不读游戏内存；地图名经 save_backup_map_name 查 Kotlin 下发表，
    // 职业名经 save_backup_class_name（native 常量表、无锁）解析。
    // 单行显示：<职业名> Lv<L> <地图名>（顺序 职业→等级→地图）；空槽「槽N (空)」；
    // 职业/等级缺失时退化（仅等级+地图 / 仅职业+地图 / 槽N+地图），不越框。
    const SlotInfo& info = g_slots[idx];
    char buf[CB_TEXT_SIZE];
    // font=3 行内垂直居中：按钮高 SLOT_ROW_H-4 = 0x44(68)、行高 0x1E(30) → 偏移 (68-30)/2 = 0x13。
    const int text_y = static_cast<int>(pt[1]) + 0x13;
    if (!info.exists) {
        snprintf(buf, sizeof(buf), "槽%d (空)", idx);
        draw_string_at_font(static_cast<int>(pt[0]) + 0x08, text_y, buf, COLOR_TEXT_GRAY,
                            SB_TEXT_FONT);
        return;
    }
    char map_short[32];
    const std::string map_name = save_backup_map_name(info.map_id);
    if (!map_name.empty()) {
        // map_name 为 UTF-8 中文（最长约 32 字节）；先截断到 16 字节，再按 font=3 宽度二次截断。
        size_t len = map_name.size() > 16 ? 16 : map_name.size();
        memcpy(map_short, map_name.c_str(), len);
        map_short[len] = 0;
    } else {
        snprintf(map_short, sizeof(map_short), "地图%d", info.map_id);
    }
    char prefix[CB_TEXT_SIZE];
    const char* cls = save_backup_class_name(info.class_idx);
    if (cls != nullptr && cls[0] != 0 && info.hero_level >= 0) {
        snprintf(prefix, sizeof(prefix), "%s Lv%d ", cls, info.hero_level);
    } else if (info.hero_level >= 0) {
        snprintf(prefix, sizeof(prefix), "Lv%d ", info.hero_level);
    } else if (cls != nullptr && cls[0] != 0) {
        snprintf(prefix, sizeof(prefix), "%s ", cls);
    } else {
        snprintf(prefix, sizeof(prefix), "槽%d ", idx);
    }
    // 可用宽 = 按钮宽 r.w - 左内距 0x08 - 右边距 0x04；地图名按剩余宽度截断到码点边界。
    const int text_avail = static_cast<int>(r.w) - 0x08 - 0x04;
    sb_truncate_utf8_font3(map_short, text_avail - sb_est_width_font3(prefix));
    snprintf(buf, sizeof(buf), "%s%s", prefix, map_short);
    draw_string_at_font(static_cast<int>(pt[0]) + 0x08, text_y, buf, UI_GOLD, SB_TEXT_FONT);
}

void ops_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int idx = -1;
    for (int i = 0; i < 3; ++i) if (ctrl == g_ops_btns[i]) { idx = i; break; }
    if (idx < 0) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const UiRect r = ops_btn_rect(idx);
    static const char* kLabels[3] = {"导出", "导入", "删除"};
    // 可用性（默认两侧都不选中 -1）：
    //   导出：选中一个存档槽。
    //   导入：槽 + 备份两侧都选中（把备份还原到该槽）。
    //   删除：恰好只选中一侧（只删那一个：槽→删该槽游戏存档；备份→删该备份 bundle）；
    //         两侧都选/都不选都不可用。
    const bool slot_sel = g_selected_slot >= 0;
    const bool backup_sel = g_selected_backup >= 0 && backup_at_page(g_selected_backup) != nullptr;
    bool enabled = false;
    if (idx == 0) enabled = slot_sel;
    else if (idx == 1) enabled = slot_sel && backup_sel;
    else enabled = (slot_sel != backup_sel);  // 恰好一侧
    // 按钮样式 = 设置页 fallback 按钮：深底 + UI_GOLD 2px 边框；不可用态用更深底色。
    // 中栏不组框，每个按钮自带边框。灰态仍可点击（事件分发不拦截），点击走原回调给出提示反馈。
    ui_custom::draw_button(ctrl, {0, 0, r.w, r.h},
                           enabled ? COLOR_BTN_BG : COLOR_BTN_BG_DIM, UI_GOLD, 2,
                           nullptr, 0, 0, 0);
    // 中栏较窄（OPS_BTN_W=0x50）：单行两字标签即可；两行 hint 会与标签重叠并横向溢出。
    // 文字在按钮框内水平居中：align=2 以按钮框中心 x = pt[0]+r.w/2 绘制（y 仍为垂直居中口径）。
    // 文字颜色随可用性：可用 UI_GOLD 金色 / 不可用 COLOR_TEXT_GRAY 灰色。
    draw_string_centered_at(static_cast<int>(pt[0] + r.w / 2), static_cast<int>(pt[1]) + 0x16,
                            kLabels[idx], enabled ? UI_GOLD : COLOR_TEXT_GRAY);
}

void list_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int local = -1;
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
        if (ctrl == g_list_btns[i]) { local = i; break; }
    }
    if (local < 0) return;
    // 空槽位（条目居中分布后本页第 n..3 格）：rect 已置空，不绘制任何占位文本。
    const UiRect r = list_btn_rect(local);
    if (r.w <= 0) return;
    // 位置用「root 绝对原点 + 计算 rect」，与命中测试口径一致（不读控件滞后 RECT）。
    int64_t pt[2];
    panel_abs_origin(r, pt);
    const BackupEntry* e = backup_at_page(local);
    if (e == nullptr) return;
    // 选中备份项：深琥珀暗色填充（ui_fill_rect_alpha，COLOR_SEL_BG(RGB565) + alpha 0x50，无描边）；
    // 暗底反衬金色首行文字，绘制在文本之前，文字浮于其上。
    if (local == g_selected_backup) {
        ui_fill_rect_alpha({pt[0], pt[1], r.w, r.h}, COLOR_SEL_BG, COLOR_SEL_BG_ALPHA);
    }
    // 文案直接取面板缓存的 g_backups 条目（backup_at_page = g_page_index*5+local，与事件
    // 命中/操作回调用同一 g_page_index 换算，口径一致）。绘制路径零 IO。
    char line1[CB_TEXT_SIZE];
    char line2[CB_TEXT_SIZE];
    // map_name 为 UTF-8 中文（最长约 32 字节）；先截断到 16 字节，再按 font=3 宽度二次截断。
    char map_short[32];
    if (!e->map_name.empty()) {
        size_t len = e->map_name.size();
        if (len > 16) len = 16;
        memcpy(map_short, e->map_name.c_str(), len);
        map_short[len] = 0;
    } else {
        snprintf(map_short, sizeof(map_short), "地图%d", e->map_id);
    }
    // 第一行：<职业名> Lv<L> <地图名>。职业名优先取 metaJson 的 class_name（导出时写入），
    // 旧备份缺失时用 class_idx 查 native 常量表；仍无则退化（仅等级+地图），不崩溃/不错位。
    char prefix[CB_TEXT_SIZE];
    const char* cls = !e->class_name.empty() ? e->class_name.c_str()
                                             : save_backup_class_name(e->class_idx);
    const int lvl = e->hero_level >= 0 ? e->hero_level : -1;
    if (cls != nullptr && cls[0] != 0 && e->hero_level >= 0) {
        snprintf(prefix, sizeof(prefix), "%s Lv%d ", cls, e->hero_level);
    } else if (e->hero_level >= 0) {
        snprintf(prefix, sizeof(prefix), "Lv%d ", e->hero_level);
    } else if (cls != nullptr && cls[0] != 0) {
        snprintf(prefix, sizeof(prefix), "%s ", cls);
    } else {
        snprintf(prefix, sizeof(prefix), "Lv%d  ", lvl);  // 未知等级旧备份：保留 Lv-1 占位
    }
    // 可用宽 = 按钮宽 r.w - 左内距 0x08 - 右边距 0x04；地图名按剩余宽度截断到码点边界。
    const int text_avail = static_cast<int>(r.w) - 0x08 - 0x04;
    sb_truncate_utf8_font3(map_short, text_avail - sb_est_width_font3(prefix));
    snprintf(line1, sizeof(line1), "%s%s", prefix, map_short);
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
    // font=3 两行块垂直居中：块高 = LIST_LINE_GAP + 行高(0x1E) = 0x3A(58)，按钮高 r.h(0x42=66)；
    // offset = (r.h - (gap+行高))/2 = (66-58)/2 = 4，两行均落在格内。
    const int text_top = static_cast<int>(pt[1]) + static_cast<int>(r.h - (LIST_LINE_GAP + SB_FONT_H)) / 2;
    draw_two_line_text_font(static_cast<int>(pt[0]) + 0x08, text_top, line1, line2, UI_GOLD,
                            COLOR_TEXT_WHITE, LIST_LINE_GAP, SB_TEXT_FONT);
}

void page_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const bool is_next = (ctrl == g_next_btn);
    const UiRect r = page_btn_rect(is_next);
    const bool disabled = is_next ? (g_page_index >= g_total_pages - 1) : (g_page_index <= 0);
    // 同操作按钮：设置页 fallback 样式（深底 + UI_GOLD 边框；禁用更深底色 + UI_GOLD 文字）
    ui_custom::draw_button(ctrl, {0, 0, r.w, r.h},
                           disabled ? COLOR_BTN_BG_DIM : COLOR_BTN_BG, UI_GOLD, 2,
                           nullptr, 0, 0, 0);
    // 标签只画「上一页/下一页」；页码指示由 root_decor_draw ⑥ 画在两按钮正中间（无框）。
    // 文字在按钮框内水平居中：align=2 以按钮框中心 x = pt[0]+r.w/2 绘制（y 仍为垂直居中口径）。
    draw_string_centered_at(static_cast<int>(pt[0] + r.w / 2), static_cast<int>(pt[1]) + 0x0C,
                            is_next ? "下一页" : "上一页", disabled ? UI_GOLD : UI_OPTION_TEXT);
}

void back_btn_draw(void* ctrl) {
    if (ctrl == nullptr || ctrl != g_back_btn) return;
    // 与设置页同款返回按钮：优先原版 option 贴图（ui_original::draw_back_button 以控件
    // 中心定位，本控件创建在屏幕左上 (8,8)、尺寸 BACK_BTN_W×BACK_BTN_H，与设置页返回
    // 按钮像素级同位同尺寸）。不依赖任何「已加载」标志：单元可能被设置页 f3 卸载、
    // 再由本面板 enter 重载，标志与实际单元状态可能不一致——直接尝试，失败退化自绘。
    // 本面板「按下即关闭」（0x17 直接触发 on_back_clicked），无按下贴图态，pressed 恒 false。
    if (ui_original::draw_back_button(ctrl, false)) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const UiRect r = back_btn_rect();
    ui_custom::draw_button(ctrl, {0, 0, r.w, r.h}, COLOR_BTN_BG, UI_GOLD, 2,
                           nullptr, 0, 0, 0);
    // 文字偏移同设置页 back fallback（text_x=8, text_y=0x18，BACK_BTN_H 高按钮的居中口径）。
    draw_string_at(static_cast<int>(pt[0]) + 8, static_cast<int>(pt[1]) + 0x18,
                   "返回", UI_OPTION_TEXT);
}

void msg_label_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    // 顶部居中显示区：直接绘制 g_msg_text（无消息时面板标题「存档备份」）。
    // 不走控件文本（fn_ctrl_btn_set_text 二次更新在此控件上不可靠），
    // 也不走 ui_draw_text_centered / fn_ui_draw_string_halign（自定义面板下不出字）。
    // 水平居中用 align=2（以 x 为中心）；此前 align=1 是右对齐（0x96adc 反汇编：
    // align==1 → 画位 = x-文本宽），信息一长就整体向左偏，与用户反馈一致。
    const char* text = g_msg_text.empty() ? kTitle : g_msg_text.c_str();
    if (text == nullptr || text[0] == 0) return;
    int64_t pt[2];
    ctrl_abs_point(ctrl, pt);
    const int64_t cx = pt[0] +
        *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(ctrl) + CO_RECT_W) / 2;
    const uint32_t color = g_msg_text.empty() ? UI_GOLD : g_msg_color;
    if (fn_grpx_set_font_color_rgb != nullptr) {
        fn_grpx_set_font_color_rgb(static_cast<int32_t>(color & 0xff),
                                   static_cast<int32_t>((color >> 8) & 0xff),
                                   static_cast<int32_t>((color >> 16) & 0xff));
    }
    if (fn_grpx_draw_string_with_font != nullptr) {
        // 垂直位置与设置页 address_draw 一致（ui_draw_text 偏移 0x18，font=1）：
        // MSG_Y=0x18 + 0x18 = 绝对 y 0x30，与设置页 addr 文本同高。
        fn_grpx_draw_string_with_font(const_cast<char*>(text),
                                      static_cast<int>(cx), static_cast<int>(pt[1]) + 0x18, 2, 1);
    }
}
