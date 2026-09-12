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
    // 与 settings_root_rect 完全同投影口径（y=0）。整体上移后内容顶 FRAME_Y=0x60 与设置页
    // 网格 GRID_Y=0x60 同高；标题/消息 MSG_Y=0x18 与设置页 addr ADDR_Y=0x18 同高（绝对 y=0x18）。
    const int64_t logical_w = screen_w * ROOT_H / screen_h;
    return {(logical_w - ROOT_W) / 2, 0, ROOT_W, ROOT_H};
}

// 当前页有效条目数（0..5）；备份列表缓存/页码变化只在持 g_sbui_mtx 时发生，
// 本函数与 list_btn_rect/DrawProc/命中测试一样都在持锁路径内读取。
int page_entry_count() {
    const int remaining =
        static_cast<int>(g_backups.size()) - g_page_index * LIST_COLS_PER_PAGE;
    if (remaining <= 0) return 0;
    return remaining > LIST_COLS_PER_PAGE ? LIST_COLS_PER_PAGE : remaining;
}

UiRect slot_btn_rect(int i) {
    // 左栏框内：首行顶 = 框内上边缘（SLOT_FRAME_Y + FRAME_BORDER=3），行 pitch SLOT_ROW_H、
    // 按钮高 SLOT_ROW_H - SLOT_ROW_GAP（行间仅留 4px 缝隙）；末行底与框内下边缘重合，
    // 数字证明见 cpp 常量区（SLOT_FRAME_H=3*ROW_H+2*3-4）。框顶 SLOT_FRAME_Y 由公共中线推出。
    // 水平左右各留 4px：按钮 x ∈ [SLOT_COL_X+4, SLOT_COL_X+SLOT_COL_W-4]，
    // 与框左右边线（3px，位于 [X, X+3] / [X+W-3, X+W]）互不重叠。
    return {SLOT_COL_X + 0x04, SLOT_FRAME_Y + FRAME_BORDER + i * SLOT_ROW_H,
            SLOT_COL_W - 0x08, SLOT_ROW_H - SLOT_ROW_GAP};
}

UiRect ops_btn_rect(int i) {
    return {OPS_COL_X + (OPS_COL_W - OPS_BTN_W) / 2,
            OPS_Y0 + i * (OPS_BTN_H + OPS_BTN_GAP),
            OPS_BTN_W, OPS_BTN_H};
}

UiRect list_btn_rect(int i) {
    // 右栏框内条目顶部对齐：首行顶 = 框内上缘（FRAME_Y+FRAME_BORDER），行距 LIST_ROW_H，
    // 不再随条目数垂直居中（n<5 时靠上排列）；末行底仅在满 5 行时贴框内下缘（框高按 5 行固定）。
    // 水平左右各留 4px（与框边线不重叠，同 slot_btn_rect 口径）。
    // 无条目的槽位 rect 置空（w=0）：命中测试恒不中、DrawProc 直接跳过。
    const int n = page_entry_count();
    if (i >= n) return {LIST_COL_X, 0, 0, 0};
    const int64_t top = FRAME_Y + FRAME_BORDER;
    return {LIST_COL_X + 0x04, top + i * LIST_ROW_H, LIST_COL_W - 0x08,
            LIST_ROW_H - LIST_ROW_GAP};
}

// 把动态 list_btn_rect 写回列表控件（创建后 / 刷新后 / 翻页后调用，调用方持 g_sbui_mtx）。
void sync_list_btn_rects_locked() {
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
        if (g_list_btns[i] != nullptr) ui_set_rect(g_list_btns[i], list_btn_rect(i));
    }
}

UiRect page_btn_rect(bool next) {
    // prev 左对齐 / next 右对齐：prev x ∈ [LIST_COL_X+0x04, +0x7C]，
    // next x ∈ [LIST_COL_X+0x116, +0x18E]（LIST_COL_W=0x192），两按钮内缘间隙 0x9A，互不重叠；
    // 页码画在正中 = LIST_COL_X+LIST_COL_W/2（见 root_decor_draw ⑥）。
    return {next ? LIST_COL_X + LIST_COL_W - LIST_PAGE_BTN_W - 0x04 : LIST_COL_X + 0x04,
            PAGE_ROW_Y, LIST_PAGE_BTN_W, LIST_PAGE_BTN_H};
}

UiRect msg_label_rect() {
    // 顶部居中（与设置页 addr 标签同位：{ROOT_W/2-0x38, 0x18, 0x120, 0x28}）。
    return {MSG_X, MSG_Y, MSG_W, MSG_H};
}

UiRect back_btn_rect() {
    // 创建时已按 root 修正为屏幕左上 (8,8)（同设置页返回按钮位置习惯）；
    // 命中测试/绘制只用 w/h（ctrl_abs_point 提供绝对坐标）。
    return g_back_btn_rect;
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

// 计算 rect 在屏幕上的绝对原点 = root 绝对原点 + 计算 rect 的 (x,y)。
// ui_create_root 以 parent=null 创建 root，故 ui_get_rect(root) 即其绝对原点；
// 该口径不读取控件自身可能滞后的 RECT，保证命中区与「实时计算 rect」严格一致。
void panel_abs_origin(const UiRect& local, int64_t out[2]) {
    UiRect rr{};
    if (g_root != nullptr && ui_get_rect(g_root, &rr)) {
        out[0] = rr.x + local.x;
        out[1] = rr.y + local.y;
    } else {
        out[0] = local.x;
        out[1] = local.y;
    }
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

    // 返回按钮：屏幕左上 (8,8)（同设置页 {8-root.x, 8-root.y} 口径），
    // 尺寸 BACK_BTN_W×BACK_BTN_H 与设置页返回按钮一致（原版贴图量级）。
    UiRect root_rect{};
    if (ui_get_rect(root, &root_rect)) {
        g_back_btn_rect = {8 - root_rect.x, 8 - root_rect.y, BACK_BTN_W, BACK_BTN_H};
    } else {
        g_back_btn_rect = {8, 8, BACK_BTN_W, BACK_BTN_H};
    }

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
    // 右栏：5 个备份项 + 翻页按钮 + 提示区
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
        void* b = ui_create_button(root, list_btn_rect(i), "", &on_btn_clicked, &list_btn_draw);
        g_list_btns[i] = b;
    }
    // 列表按钮 rect 随当前页条目数垂直居中分布：创建时缓存可能已有数据（self_check），
    // 创建后按最新 page_entry_count 同步一次（后续刷新/翻页由 sync_list_btn_rects_locked 维护）。
    sync_list_btn_rects_locked();
    g_prev_btn = ui_create_button(root, page_btn_rect(false), "", &on_btn_clicked, &page_btn_draw);
    g_next_btn = ui_create_button(root, page_btn_rect(true), "", &on_btn_clicked, &page_btn_draw);
    // 「← 返回」：唯一关闭入口（BACK 不触达 PopupState）。
    // 位置习惯与设置页一致：屏幕左上 (8,8)，原版贴图样式；命中测试随 g_back_btn_rect 走。
    g_back_btn = ui_create_button(root, g_back_btn_rect, "", &on_back_clicked, &back_btn_draw);
    g_msg_label = ui_create_label(root, msg_label_rect(), "", &msg_label_draw);
    // 控件文本 = 当前提示或标题（msg_label_draw 用 ui_draw_text_centered 绘制控件文本）。
    if (g_msg_label != nullptr && fn_ctrl_btn_set_text != nullptr) {
        fn_ctrl_btn_set_text(g_msg_label,
                             g_msg_text.empty() ? const_cast<char*>(kTitle)
                                                : const_cast<char*>(g_msg_text.c_str()));
    }
    g_root = root;
    SB_UI_LOG("create controls: root=%p", root);
}

void savebackup_panel_enter() {
    const auto t0 = std::chrono::steady_clock::now();
    SB_UI_LOG("savebackup panel enter");
    // 槽位刷新必须在游戏线程（enter 回调即游戏线程）执行：SAVE_CreateSaveSlot 走原版保存链，
    // 后台线程调用会崩溃（见 refresh_slots_on_game_thread 注释）。
    refresh_slots_on_game_thread();
    // 视觉资源（与设置页同款）：背景贴图两半 + 原版返回按钮贴图；resumed 分支同样补加载。
    // option 贴图单元与设置页共用：无独立标志，每次 enter 无条件确保加载
    // （fn_imgsys_unit_load 幂等；设置页 f3 卸载后由本面板 enter 重载）。
    bool resumed = false;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        ui_original::load_option_images();
        if (!g_title_background_images_loaded) {
            g_title_background_images_loaded = ui_load_image_unit(TITLE_BACKGROUND_LEFT_UNIT) &&
                ui_load_image_unit(TITLE_BACKGROUND_RIGHT_UNIT);
        }
        if (g_root != nullptr) {
            // 控件树已存在（从隐藏栈回来），仅恢复活动标志；数据走后台刷新。
            g_panel_active = true;
            g_close_delay_frames = 0;
            g_back_pressed = 0;
            // 页码/条目缓存可能在隐藏期间被 self_check/刷新改动，列表按钮 rect 重对齐。
            sync_list_btn_rects_locked();
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
    // 控件 rect 写回只在游戏线程（本函数）执行：后台刷新线程不直接 ui_set_rect。
    // 列表条目数/页码变化后在此把最新 rect 写回控件，使引擎侧的控件 rect 与计算 rect 一致
    // （命中/绘制另用 panel_abs_origin + 计算 rect，双保险）。
    sync_list_btn_rects_locked();
    // 帧封装与 game_ui_settings_panel_process 同款（ui_begin_frame 内含 SaveLCD/RestoreLCD）
    ui_begin_frame();
    // 背景/遮罩/分割线/标题（设置页同款视觉语言，见 game_ui_savebackup_render.inc）
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
    ui_end_frame();
}

void savebackup_panel_f3() {
    SB_UI_LOG("savebackup panel f3 (terminate)");
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    g_panel_active = false;
    g_close_delay_frames = 0;
    g_back_pressed = 0;
    // 不卸载任何共享贴图单元：kOptionImageUnit(0x59) 与设置页返回按钮/切换开关共用，
    // TITLE_BACKGROUND_* (0x4f/0x50) 与主菜单标题/设置页共用——本面板不是所有者。
    // 此前在这里 unload_option_images() 会把仍在栈中的设置页要用的贴图卸载，
    // 返回设置页后 back_btn_draw 取不到贴图退化为「方框+文字」（用户实测 bug）；
    // 卸载 TITLE_BACKGROUND_* 还会导致主菜单重绘 GRPX_DrawPart 空指针崩溃（tombstone_30）。
    // 卸载职责在所有者（settings_panel_f3 / 主菜单）。
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
            // 命中测试：用「root 绝对原点 + 实时计算 rect」，不读控件自身可能滞后的 RECT。
            // 这样命中区与绘制（同用计算 rect）严格一致，列表/槽等动态 rect 也稳定命中。
            auto hit = [&](void* ctrl, UiRect r) -> bool {
                if (ctrl == nullptr || r.w <= 0 || r.h <= 0) return false;
                int64_t p[2];
                panel_abs_origin(r, p);
                return tx >= p[0] && tx < p[0] + r.w && ty >= p[1] && ty < p[1] + r.h;
            };
            // 槽位：纯状态修改，持锁内联完成。再次点击已选槽 → 取消选中（-1）；
            // 切换/取消都清除待确认并清提示（与选中语义一致）。
            for (int i = 0; i < 3; ++i) {
                if (hit(g_slot_btns[i], slot_btn_rect(i))) {
                    g_selected_slot = (g_selected_slot == i) ? -1 : i;
                    if (g_pending_op != 0) clear_pending_locked();
                    set_msg_locked("", UI_OPTION_TEXT, false);
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
                // 列表项：纯状态修改，持锁内联完成。再次点击已选备份项 → 取消选中（-1）；
                // 切换/取消都清除待确认并清提示。
                for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
                    if (hit(g_list_btns[i], list_btn_rect(i))) {
                        if (backup_at_page(i) != nullptr) {
                            g_selected_backup = (g_selected_backup == i) ? -1 : i;
                            if (g_pending_op != 0) {
                                clear_pending_locked();
                                set_msg_locked("", UI_OPTION_TEXT, false);
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
