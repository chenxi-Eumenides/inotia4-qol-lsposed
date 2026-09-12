// 面板几何 + 控件创建 + 事件分发
//
// 复用 ui_create_root/ui_create_button/ui_create_label：root 不入控件树；
// 三栏每项是一个 button（参与命中，但 DrawProc 自绘高亮/边框/多行文本）。
// 触摸事件在 PopupState event 内自分发（event=0x17 按下 → 命中测试 → 触发点击回调）。

// 检查游戏 popup state stack 顶是否是 savebackup entry（v0.7.x）。
// 用于嵌套 push 后校验栈顶是否真的切换：fn_popupstate_push 内部 ArrayStack_Push 失败时
// GetTop 会返回当前栈顶指针（settings entry），与"push 成功"返回的指针一样非 0，
// 仅凭返回值无法区分；必须直接读栈顶 enter_func 字段与 &savebackup_panel_enter 对比。
// 注：data_popup_top_vma 只能解析 g_base+VMA 的原版 enter，无法识别 wrapper 函数。
bool is_savebackup_at_stack_top() {
    if (g_popup_stack == nullptr || g_base == 0) return false;
    uint8_t* stk = reinterpret_cast<uint8_t*>(g_popup_stack);
    uint32_t count = *reinterpret_cast<uint32_t*>(stk + POPUP_STACK_COUNT);
    if (count == 0 || count > 27) return false;
    uint64_t data = *reinterpret_cast<uint64_t*>(stk + POPUP_STACK_DATA);
    if (data == 0) return false;
    uint8_t* top = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data))
                   + (count - 1) * POPUP_STATE_SIZE;
    uintptr_t enter = *reinterpret_cast<uintptr_t*>(top + POPUP_ENTRY_ENTER);
    return enter == reinterpret_cast<uintptr_t>(&savebackup_panel_enter);
}

UiRect panel_root_rect() {
    const int64_t screen_w = ROOT_W + static_cast<int64_t>(fn_calc_res_width()) * 2;
    const int64_t screen_h = ROOT_H + static_cast<int64_t>(fn_calc_res_height()) * 2;
    if (screen_w <= 0 || screen_h <= 0) return {0, 0, ROOT_W, ROOT_H};
    const int64_t logical_w = screen_w * ROOT_H / screen_h;
    return {(logical_w - ROOT_W) / 2, 0, ROOT_W, ROOT_H};
}

UiRect slot_btn_rect(int i) {
    return {CONTENT_X, SLOT_Y + i * SLOT_ROW_H, SLOT_COL_W, SLOT_ROW_H - 0x04};
}

UiRect ops_btn_rect(int i) {
    return {OPS_COL_X + (OPS_COL_W - OPS_BTN_W) / 2,
            OPS_Y0 + i * (OPS_BTN_H + OPS_BTN_GAP),
            OPS_BTN_W, OPS_BTN_H};
}

UiRect list_btn_rect(int i) {
    return {LIST_COL_X, CONTENT_Y + 0x08 + i * LIST_ROW_H,
            LIST_COL_W, LIST_ROW_H - 0x04};
}

UiRect page_btn_rect(bool next) {
    return {LIST_COL_X, CONTENT_Y + CONTENT_H - LIST_PAGE_BTN_H - 0x06,
            LIST_PAGE_BTN_W, LIST_PAGE_BTN_H};
}

UiRect msg_label_rect() {
    return {LIST_COL_X + LIST_PAGE_BTN_W + GAP,
            CONTENT_Y + CONTENT_H - LIST_PAGE_BTN_H - 0x06,
            LIST_COL_W - LIST_PAGE_BTN_W - GAP * 2, LIST_PAGE_BTN_H};
}

UiRect back_btn_rect() {
    return {CONTENT_X, CONTENT_Y + CONTENT_H + 0x04,
            0x60, HINT_H - 0x08};
}

void ctrl_abs_point(void* ctrl, int64_t out[2]) {
    uint8_t* c = reinterpret_cast<uint8_t*>(ctrl);
    int64_t x = *reinterpret_cast<int64_t*>(c + CO_RECT_X);
    int64_t y = *reinterpret_cast<int64_t*>(c + CO_RECT_Y);
    void* p = *reinterpret_cast<void**>(c + CO_PARENT);
    while (p != nullptr) {
        uint8_t* pc = reinterpret_cast<uint8_t*>(p);
        x += *reinterpret_cast<int64_t*>(pc + CO_RECT_X);
        y += *reinterpret_cast<int64_t*>(pc + CO_RECT_Y);
        p = *reinterpret_cast<void**>(pc + CO_PARENT);
    }
    out[0] = x;
    out[1] = y;
}

void destroy_panel_controls_locked() {
    // 控件由 root 创建但无 TouchHandle 注册，递归删除会 SIGBUS → 不调 TouchHandle_DeleteControl；
    // 仅清字段。f3 阶段 root 自身仍复用（见 custom_panel 同模式）。
    for (int i = 0; i < 3; ++i) {
        g_slot_btns[i] = nullptr;
        g_ops_btns[i] = nullptr;
    }
    for (int i = 0; i < LIST_COLS_MAX; ++i) g_list_btns[i] = nullptr;
    g_prev_btn = nullptr;
    g_next_btn = nullptr;
    g_back_btn = nullptr;
    g_msg_label = nullptr;
    g_root = nullptr;
}

void create_panel_controls_locked() {
    void* root = ui_create_root(panel_root_rect());
    if (root == nullptr) {
        SB_UI_LOG("create root failed");
        return;
    }
    // root 的 CO_CONTROL_PROC 清零（防 GetData=null SIGSEGV）
    *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(root) + CO_CONTROL_PROC) = 0;

    // 左栏：3 个存档槽按钮。自绘按钮文本一律传空串：文字只由自定义 DrawProc 绘制，
    // 避免与 fn_ctrl_btn_draw 的按钮自身文本重叠（与设置页「toggle 用默认文本、
    // desc label 自绘文本」的分工口径一致：文本只在一处绘制）。
    for (int i = 0; i < 3; ++i) {
        void* b = ui_create_button(root, slot_btn_rect(i), "", &on_btn_clicked, &slot_btn_draw);
        g_slot_btns[i] = b;
    }
    // 中栏：3 个操作按钮（导出 / 导入 / 删除）
    for (int i = 0; i < 3; ++i) {
        void* b = ui_create_button(root, ops_btn_rect(i), "", &on_btn_clicked, &ops_btn_draw);
        g_ops_btns[i] = b;
    }
    // 右栏：4 个备份项 + 翻页按钮 + 提示区
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
        void* b = ui_create_button(root, list_btn_rect(i), "", &on_btn_clicked, &list_btn_draw);
        g_list_btns[i] = b;
    }
    g_prev_btn = ui_create_button(root, page_btn_rect(false), "", &on_btn_clicked, &page_btn_draw);
    g_next_btn = ui_create_button(root, page_btn_rect(true), "", &on_btn_clicked, &page_btn_draw);
    // 底部提示区左侧「← 返回」：唯一关闭入口（BACK 不触达 PopupState）。
    g_back_btn = ui_create_button(root, back_btn_rect(), "", &on_back_clicked, &back_btn_draw);
    g_msg_label = ui_create_label(root, msg_label_rect(), "", &msg_label_draw);
    g_root = root;
    SB_UI_LOG("create controls: root=%p", root);
}

void savebackup_panel_enter() {
    const auto t0 = std::chrono::steady_clock::now();
    SB_UI_LOG("savebackup panel enter");
    // 槽位刷新必须在游戏线程（enter 回调即游戏线程）执行：SAVE_CreateSaveSlot 走原版保存链，
    // 后台线程调用会崩溃（见 refresh_slots_on_game_thread 注释）。
    refresh_slots_on_game_thread();
    bool resumed = false;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_root != nullptr) {
            // 控件树已存在（从隐藏栈回来），仅恢复活动标志；数据走后台刷新。
            g_panel_active = true;
            g_close_delay_frames = 0;
            g_back_pressed = 0;
            resumed = true;
        } else if (g_base == 0 || fn_calc_res_width == nullptr || fn_calc_res_height == nullptr) {
            SB_UI_LOG("savebackup panel enter: symbols not resolved");
            return;
        } else {
            create_panel_controls_locked();
            g_panel_active = true;
            g_close_delay_frames = 0;
            g_back_pressed = 0;
        }
    }
    // 锁外触发后台刷新：列表目录 IO + 三槽读取不占游戏线程（v0.7.x 曾同步执行
    // refresh_backups_locked，冷 IO 下 enter → 控件创建实测延迟 13.4s）。
    // 「每次进入读目录」语义保留：异步线程本轮结束还会补跑 queued 请求。
    refresh_panel_data_async();
    const auto t1 = std::chrono::steady_clock::now();
    SB_UI_LOG("savebackup panel enter %s in %.1f ms",
              resumed ? "resumed" : "controls created",
              std::chrono::duration<double, std::milli>(t1 - t0).count());
}

void savebackup_panel_process() {
    if (!g_panel_active) return;
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    if (g_root == nullptr || g_base == 0) return;
    if (g_close_delay_frames > 0) {
        --g_close_delay_frames;
        if (g_close_delay_frames == 0 && fn_ui_set_popup_process_info != nullptr) {
            fn_ui_set_popup_process_info(3, 0);
            return;
        }
    }
    if (fn_grpx_start == nullptr || fn_grpx_end == nullptr) return;
    // SaveLCD/RestoreLCD 与 game_ui_custom_panel 同源
    if (fn_ui_get_refresh_lcd_flag != nullptr && fn_ui_set_refresh_lcd_flag != nullptr) {
        if (fn_ui_get_refresh_lcd_flag()) {
            if (fn_grp_save_lcd != nullptr) fn_grp_save_lcd();
            fn_ui_set_refresh_lcd_flag(0);
        } else {
            if (fn_grp_restore_lcd != nullptr) fn_grp_restore_lcd();
        }
    }
    fn_grpx_start();
    root_decor_draw();
    if (fn_ctrl_btn_draw != nullptr) {
        for (int i = 0; i < 3; ++i) if (g_slot_btns[i]) fn_ctrl_btn_draw(g_slot_btns[i]);
        for (int i = 0; i < 3; ++i) if (g_ops_btns[i]) fn_ctrl_btn_draw(g_ops_btns[i]);
        for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) if (g_list_btns[i]) fn_ctrl_btn_draw(g_list_btns[i]);
        if (g_prev_btn) fn_ctrl_btn_draw(g_prev_btn);
        if (g_next_btn) fn_ctrl_btn_draw(g_next_btn);
        if (g_back_btn) fn_ctrl_btn_draw(g_back_btn);
        if (g_msg_label) fn_ctrl_btn_draw(g_msg_label);
    }
    fn_grpx_end();
}

void savebackup_panel_f3() {
    SB_UI_LOG("savebackup panel f3 (terminate)");
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    g_panel_active = false;
    g_close_delay_frames = 0;
    g_back_pressed = 0;
    // 控件树保留复用（同 game_ui_custom_panel：递归删除 SIGBUS）
}

void savebackup_panel_f4() {}

uint32_t savebackup_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (g_root == nullptr || param == 0) return 1;
    if (g_close_delay_frames > 0) return 1;
    if (event == 0x17) {
        int64_t tx = *reinterpret_cast<const int64_t*>(param);
        int64_t ty = *reinterpret_cast<const int64_t*>(param + 8);
        // 需要锁外处理的按钮（操作按钮/翻页）：on_btn_clicked 契约为「调用方不持锁」，
        // 内部按需拿 g_sbui_mtx；导入/删除确认执行路径要锁外做 IO 并调
        // refresh_slots_on_game_thread()（其内部自加锁，持锁调用会死锁，g_sbui_mtx 非递归）。
        void* clicked_ctrl = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_sbui_mtx);
            if (!g_panel_active) return 1;
            // 命中测试（按控件遍历）
            auto hit = [&](void* ctrl, UiRect r) -> bool {
                if (ctrl == nullptr) return false;
                int64_t p[2];
                ctrl_abs_point(ctrl, p);
                return tx >= p[0] && tx < p[0] + r.w && ty >= p[1] && ty < p[1] + r.h;
            };
            // 槽位：纯状态修改，持锁内联完成（切换槽位同时取消待确认）。
            for (int i = 0; i < 3; ++i) {
                if (hit(g_slot_btns[i], slot_btn_rect(i))) {
                    g_selected_slot = i;
                    if (g_pending_op != 0) clear_pending_locked();
                    set_msg_locked("", COLOR_TEXT, false);
                    return 1;
                }
            }
            // 操作按钮：记录指针，锁外调用（导入/删除为长 IO，不得持 g_sbui_mtx）。
            for (int i = 0; i < 3; ++i) {
                if (hit(g_ops_btns[i], ops_btn_rect(i))) {
                    clicked_ctrl = g_ops_btns[i];
                    break;
                }
            }
            if (clicked_ctrl == nullptr) {
                // 列表项：纯状态修改，持锁内联完成（切换选中同时取消待确认）。
                for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
                    if (hit(g_list_btns[i], list_btn_rect(i))) {
                        if (backup_at_page(i) != nullptr) {
                            g_selected_backup = i;
                            if (g_pending_op != 0) {
                                clear_pending_locked();
                                set_msg_locked("", COLOR_TEXT, false);
                            }
                        }
                        return 1;
                    }
                }
                // 翻页：同样锁外交给 on_btn_clicked。
                if (hit(g_prev_btn, page_btn_rect(false))) {
                    clicked_ctrl = g_prev_btn;
                } else if (hit(g_next_btn, page_btn_rect(true))) {
                    clicked_ctrl = g_next_btn;
                } else if (hit(g_back_btn, back_btn_rect())) {
                    // 返回：锁外交给 on_back_clicked（其内部调 fn_ui_set_popup_process_info
                    // 触发 pop → savebackup_panel_f3 拿 g_sbui_mtx，持锁调用会死锁）。
                    clicked_ctrl = g_back_btn;
                }
            }
        }
        if (clicked_ctrl != nullptr) {
            on_btn_clicked(clicked_ctrl);  // 游戏线程，锁外
        }
        return 1;
    } else if (event == 0x18 && g_back_pressed != 0) {
        // 抬起事件无返回按钮：在自绘面板中，「关闭」通过单独的 on_btn_clicked 触发；保留兼容。
        g_back_pressed = 0;
    }
    return 1;  // 吞掉触摸
}
