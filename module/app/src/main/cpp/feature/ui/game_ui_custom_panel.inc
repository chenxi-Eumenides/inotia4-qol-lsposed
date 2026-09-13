// ---- 格子按钮回调 ----

void cell0_log_clicked(void* ctrl) {
    CUSTOM_LOG("CUSTOM-PANEL: cell0 log button clicked (ctrl=%p)", ctrl);
}

void cell1_stack_clicked(void* ctrl) {
    bool target = !stack_limit_enabled();
    bool ok = set_stack_limit_enabled(target);
    CUSTOM_LOG("CUSTOM-PANEL: cell1 stack limit %s -> %s", target ? "ON" : "OFF", ok ? "ok" : "failed");
    if (ok && g_cells[1].btn != nullptr && fn_ctrl_btn_set_text != nullptr) {
        static char t[CB_TEXT_SIZE];
        snprintf(t, sizeof(t), "堆叠上限:%s", target ? "开" : "关");
        fn_ctrl_btn_set_text(g_cells[1].btn, t);
    }
}

void cell2_close_clicked(void* ctrl) {
    CUSTOM_LOG("CUSTOM-PANEL: cell2 close clicked");
    if (fn_ui_set_popup_process_info == nullptr) return;
    fn_ui_set_popup_process_info(3, 0);  // 弹出自定义面板
    fn_ui_set_popup_process_info(3, 0);  // 弹出商店
}

// ---- 绘制回调（DrawProc，x0=ctrl；ControlButton_Draw 0xaac2c 分发调用）----

void custom_btn_draw(void* ctrl) {
    if (g_base == 0) return;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctrl) + CO_DATA);
    if (data == nullptr) return;
    int64_t rect[2];
    ctrl_abs_point(ctrl, rect);
    static int dbg = 0;
    if (++dbg % 60 == 1) {
        // font 来源实证：GOT 槽 0x2f3000+0x628（font_id 路径）、字体对象槽 0x2f3000+0xf88
        void** slot_f88 = reinterpret_cast<void**>(g_base + G_FONT_OBJ_GOT_VMA);
        void* font_obj = (slot_f88 != nullptr) ? *slot_f88 : nullptr;
        int f88_first = 0;
        if (font_obj != nullptr) f88_first = *reinterpret_cast<int*>(font_obj);
        QOL_LOG_DEBUG(QolDomain::kUi,
                      "btn_draw: data=%s rect=(%lld,%lld) font_id=%d f88_obj=%p f88[0]=%d",
                   reinterpret_cast<char*>(data),
                   static_cast<long long>(rect[0]), static_cast<long long>(rect[1]),
                   font_id(), font_obj, f88_first);
    }
    // GRPX_DrawPart（贴图）在 POPUPSTATE_Process（MainProcess 阶段，非 GL 绘制帧）调用会
    // SIGSEGV（真机实测，纹理 GL 状态未就绪）→ 降级为色块背景 + 文本，布局/功能先行验证。
    // GRPX_FillRectAlpha 的 alpha>0x64(100) 直接 return 不绘制（反汇编 8fcd0）→ 用 FillRect（alpha 嵌 ABGR 色值）
    if (fn_grpx_fill_rect != nullptr) {
        fn_grpx_fill_rect(static_cast<int>(rect[0]), static_cast<int>(rect[1]),
                          BTN_W, BTN_H, 0xFF606060);  // ABGR 灰按钮底
    }
    if (fn_grpx_set_font_color != nullptr) fn_grpx_set_font_color(0xFFFFFFFF);
    if (fn_ui_draw_string_halign != nullptr && data[0] != 0) {
        fn_ui_draw_string_halign(reinterpret_cast<char*>(data),
                                 static_cast<int>(rect[0]) + 8, static_cast<int>(rect[1]) + 14,
                                 font_id(), 1);
    }
}

void custom_desc_draw(void* ctrl) {
    if (g_base == 0) return;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctrl) + CO_DATA);
    if (data == nullptr || data[0] == 0 || fn_ui_draw_string_halign == nullptr) return;
    int64_t rect[2];
    ctrl_abs_point(ctrl, rect);
    if (fn_grpx_set_font_color != nullptr) fn_grpx_set_font_color(0xFFC8C8C8);
    fn_ui_draw_string_halign(reinterpret_cast<char*>(data),
                             static_cast<int>(rect[0]) + 2, static_cast<int>(rect[1]) + 2,
                             font_id(), 1);
}

// ---- state list 条目回调（注入 INAP 条目，IAP 已被模块屏蔽=死条目）----

void custom_panel_enter() {
    CUSTOM_LOG("custom panel enter");
    if (g_root != nullptr) {
        g_panel_active = true;
        return;
    }
    if (g_base == 0 || fn_ctrl_create == nullptr || fn_ctrl_btn_create == nullptr ||
        fn_ctrl_btn_set_text == nullptr || fn_ctrl_btn_set_draw_proc == nullptr ||
        fn_ctrl_set_active == nullptr ||
        fn_calc_res_width == nullptr || fn_calc_res_height == nullptr) {
        CUSTOM_LOG("custom panel enter: symbols not resolved");
        return;
    }
    // UI_CreateGroupBaseControl 要求 parent 非空（反汇编 cbz x0 → ret 0）→ 用 ControlObject_Create
    // 建独立根（type=0 组容器），手动补 Proc（TouchHandle 分发）+ Active（CreateControlInfo 已置 0x20）。
    void* root = fn_ctrl_create(0, nullptr, nullptr, nullptr);
    if (root == nullptr) {
        CUSTOM_LOG("custom panel enter: root create failed");
        return;
    }
    set_ctrl_rect(root, fn_calc_res_width(), fn_calc_res_height(), ROOT_W, ROOT_H);
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(root) + CO_TYPE) = 0;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(root) + CO_ACTIVE) = 0x20;
    *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(root) + CO_PROC) =
        g_base + F_TOUCH_HANDLE_CONTROL_EVENT_PROC_VMA;
    // root 无数据块：CO_CONTROL_PROC 若指向 ControlButton_ControlEventProc，
    // 触摸选中时 GetData=null → [data+0x70] SIGSEGV（真机实测）→ 清零，不参与游戏触摸链
    *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(root) + CO_CONTROL_PROC) = 0;
    static const char* kBtnText[3] = {"写日志", "堆叠上限:关", "关闭面板"};
    static const char* kDescText[3] = {"点击写入一条日志", "切换999堆叠补丁", "返回并关闭商店"};
    static void (*const kBtnProc[3])(void*) = {cell0_log_clicked, cell1_stack_clicked, cell2_close_clicked};
    const int col_x[2] = {CELL_COL0_X, CELL_COL1_X};
    const int row_y[3] = {CELL_ROW0_Y, CELL_ROW1_Y, CELL_ROW2_Y};
    for (int i = 0; i < 3; ++i) {
        g_cell_procs[i] = kBtnProc[i];
        int bx = col_x[i % 2];
        int by = row_y[i / 2];
        void* btn = fn_ctrl_btn_create(root, reinterpret_cast<void*>(kBtnProc[i]));
        if (btn == nullptr) continue;
        set_ctrl_rect(btn, bx, by, BTN_W, BTN_H);
        fn_ctrl_btn_set_text(btn, const_cast<char*>(kBtnText[i]));
        fn_ctrl_btn_set_draw_proc(btn, reinterpret_cast<void*>(&custom_btn_draw));
        void* desc = fn_ctrl_btn_create(root, nullptr);
        if (desc == nullptr) continue;
        set_ctrl_rect(desc, bx, by + BTN_H + 8, DESC_W, DESC_H);
        fn_ctrl_btn_set_text(desc, const_cast<char*>(kDescText[i]));
        fn_ctrl_btn_set_draw_proc(desc, reinterpret_cast<void*>(&custom_desc_draw));
        fn_ctrl_set_active(desc, 0);  // 说明文本不参与触摸
        g_cells[i].btn = btn;
        g_cells[i].desc = desc;
    }
    g_root = root;
    g_panel_active = true;
    CUSTOM_LOG("custom panel enter: root=%p 3 cells created", root);
}

void custom_panel_process() {
    if (!g_panel_active || g_root == nullptr || g_base == 0) return;
    if (fn_grpx_start == nullptr || fn_grpx_end == nullptr) return;
    static int dbg_frame = 0;
    ++dbg_frame;
    if (qol_log_debug_enabled()) {
        int64_t pt[2];
        ctrl_abs_point(g_root, pt);
        QOL_LOG_DEBUG_EVERY(QolDomain::kUi, 60,
                            "process: frame=%d root_abs=(%lld,%lld) cells ok=%d/%d", dbg_frame,
                            static_cast<long long>(pt[0]), static_cast<long long>(pt[1]),
                            g_cells[0].btn != nullptr, g_cells[2].btn != nullptr);
    }
    // 模拟 Scene_Draw_POPUP_SC_STORE 的 LCD 结构（绘制才进入渲染）：
    // 完整路径(RefreshLCDFlag=1): SaveLCD + 清标志；快路径(=0): RestoreLCD 恢复背景缓冲
    if (fn_ui_get_refresh_lcd_flag != nullptr && fn_ui_set_refresh_lcd_flag != nullptr) {
        if (fn_ui_get_refresh_lcd_flag()) {
            if (fn_grp_save_lcd != nullptr) fn_grp_save_lcd();
            fn_ui_set_refresh_lcd_flag(0);
        } else {
            if (fn_grp_restore_lcd != nullptr) fn_grp_restore_lcd();
        }
    }
    fn_grpx_start();
    // GRPX_FillRect 颜色=ABGR（0xFFFF0000 显示为蓝，真机实测）→ 面板色用 ABGR
    // 遮罩用 FillRectAlpha（alpha 0x3c<=0x64 有效，反汇编 8fcd0 验证）
    if (fn_grpx_fill_rect_alpha != nullptr) {
        fn_grpx_fill_rect_alpha(231, 0, ROOT_W, ROOT_H, 0xFF000000, 0x3c);  // 半透明黑遮罩
    }
    for (int i = 0; i < 3; ++i) {
        if (g_cells[i].btn != nullptr && fn_ctrl_btn_draw != nullptr) {
            fn_ctrl_btn_draw(g_cells[i].btn);
        }
        if (g_cells[i].desc != nullptr && fn_ctrl_btn_draw != nullptr) {
            fn_ctrl_btn_draw(g_cells[i].desc);
        }
    }
    fn_grpx_end();
}

void custom_panel_f4() {}  // noop：绘制在 process（官方 Scene_Draw 结构），f4 是 GRP 背景层会被覆盖

void custom_panel_f3() {
    CUSTOM_LOG("custom panel f3 (terminate)");
    g_panel_active = false;
    // 不调 TouchHandle_DeleteControl：root 为 ControlObject_Create 独立创建（无完整父链/
    // 触摸注册），游戏递归删除遍历链表 SIGBUS（真机实测 tombstone_17）→ 控件树保留复用，
    // 下次 enter 直接沿用（g_root 非空分支），进程重启时由系统释放。
}

uint32_t custom_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (g_root == nullptr || param == 0) return 1;
    // 不转发 fn_touch_handle_event：游戏触摸链选中 root/按钮会调 ControlButton_ControlEventProc，
    // 而 root 无数据块（GetData=null，+0x70 读 SIGSEGV 实测）→ 自实现命中测试。
    // param = 触摸结构 {i64 x, i64 y, i64 id}（TouchHandle_Event 0x17 分支 a3a00 拷贝 24B）
    if (event == 0x17) {
        int64_t tx = *reinterpret_cast<const int64_t*>(param);
        int64_t ty = *reinterpret_cast<const int64_t*>(param + 8);
        for (int i = 0; i < 3; ++i) {
            void* btn = g_cells[i].btn;
            if (btn == nullptr || g_cell_procs[i] == nullptr) continue;
            int64_t pt[2];
            ctrl_abs_point(btn, pt);
            if (tx >= pt[0] && tx < pt[0] + BTN_W && ty >= pt[1] && ty < pt[1] + BTN_H) {
                CUSTOM_LOG("event: cell%d hit tx=%lld ty=%lld rect=(%lld,%lld)", i,
                           static_cast<long long>(tx), static_cast<long long>(ty),
                           static_cast<long long>(pt[0]), static_cast<long long>(pt[1]));
                g_cell_procs[i](btn);
                break;
            }
        }
    }
    return 1;  // 已处理（吞掉触摸，不穿透到底层）
}

