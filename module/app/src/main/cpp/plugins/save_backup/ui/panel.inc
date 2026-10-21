// 面板几何、core.custom_ui 组件创建与组件事件派发。

// 检查游戏 popup state stack 顶是否是 savebackup entry（v0.7.x）。
// 用于嵌套 push 后校验栈顶是否真的切换：fn_popupstate_push 内部 ArrayStack_Push 失败时
// GetTop 会返回当前栈顶指针（settings entry），与"push 成功"返回的指针一样非 0，
// 仅凭返回值无法区分；必须直接读栈顶 enter_func 字段与 &savebackup_panel_enter 对比。
// 注：popup_top_vma 只能解析 g_base+VMA 的原版 enter，无法识别 wrapper 函数。
bool is_savebackup_at_stack_top() {
    return ui_popup_top_matches(&savebackup_panel_enter);
}

UiRect panel_root_rect() {
    int32_t inset_width = 0;
    int32_t inset_height = 0;
    if (!ui_resolution_width(&inset_width) || !ui_resolution_height(&inset_height)) {
        return {0, 0, ROOT_W, ROOT_H};
    }
    const int64_t screen_w = ROOT_W + static_cast<int64_t>(inset_width) * 2;
    const int64_t screen_h = ROOT_H + static_cast<int64_t>(inset_height) * 2;
    if (screen_w <= 0 || screen_h <= 0) return {0, 0, ROOT_W, ROOT_H};
    // 与 settings_root_rect 完全同投影口径（y=0）。整体上移后内容顶 FRAME_Y=0x60 与设置页
    // 网格 GRID_Y=0x60 同高；标题/消息 MSG_Y=0x18 与设置页 header HEADER_Y=0x18 同高（绝对 y=0x18）。
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

UiRect auto_btn_rect() { return {AUTO_X + 0x04, AUTO_Y, LIST_COL_W - 0x08, AUTO_H}; }

// 把动态 list_btn_rect 写回列表控件（创建后 / 刷新后 / 翻页后调用，调用方持 g_sbui_mtx）。
void sync_list_btn_rects_locked() {
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
        if (g_list_btns[i] != nullptr && g_ui_components != nullptr && g_ui_components->set_rect != nullptr) {
            UiRect rect = list_btn_rect(i);
            if (rect.w <= 0 || rect.h <= 0) rect = {LIST_COL_X, 0, 1, 1};
            (void)g_ui_components->set_rect(g_list_btns[i], rect);
        }
    }
}

// 计算 rect 在屏幕上的绝对原点 = 组件面板绝对原点 + 面板相对坐标。
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
    void* native_root = g_root;
    if (native_root != nullptr && g_ui_components != nullptr && g_ui_components->destroy != nullptr)
        (void)g_ui_components->destroy(native_root);
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (native_root != nullptr && ui != nullptr && ui->delete_control != nullptr)
        (void)ui->delete_control(native_root);
    for (int i = 0; i < 3; ++i) {
        g_slot_btns[i] = nullptr;
        g_ops_btns[i] = nullptr;
    }
    for (int i = 0; i < LIST_COLS_MAX; ++i) g_list_btns[i] = nullptr;
    g_auto_btn = nullptr;
    g_prev_btn = nullptr;
    g_next_btn = nullptr;
    g_back_btn = nullptr;
    g_root = nullptr;
}

void create_panel_controls_locked() {
    if (g_ui_components == nullptr || g_ui_components->panel_create == nullptr ||
        g_ui_components->button_create == nullptr ||
        g_ui_components->nav_button_create == nullptr ||
        g_ui_components->arrow_button_create == nullptr) {
        SB_UI_LOG("create controls failed at component API validation");
        return;
    }
    const UiRect root_rect = panel_root_rect();
    const UiRect mask{0, 0, ROOT_W + root_rect.x * 2, ROOT_H};
    const contracts::UiPanelDesc root_desc{root_rect, mask, 0, 0};
    void* root = nullptr;
    if (!contracts::succeeded(g_ui_components->panel_create(&root_desc, nullptr, &root)) || root == nullptr) {
        SB_UI_LOG("create controls failed at root panel_create");
        return;
    }
    g_root = root;
    const auto create_hit = [&](UiRect rect, int kind, int index, void* parent, void** out) {
        const contracts::UiButtonDesc desc{rect, "", contracts::kUiTextButtonFontDefault,
            &component_action_clicked, reinterpret_cast<void*>(static_cast<intptr_t>(kind + index))};
        return contracts::succeeded(g_ui_components->button_create(&desc, parent, out)) && *out != nullptr;
    };
    for (int i = 0; i < 3; ++i) {
        if (!create_hit(slot_btn_rect(i), 100, i, root, &g_slot_btns[i])) {
            SB_UI_LOG("create controls failed at slot button %d", i);
            return;
        }
    }
    const contracts::UiArrowButtonDesc export_arrow{ops_btn_rect(0), contracts::kUiArrowRight, 1,
        &component_action_clicked, reinterpret_cast<void*>(400)};
    if (!contracts::succeeded(g_ui_components->arrow_button_create(&export_arrow, root, &g_ops_btns[0]))) {
        SB_UI_LOG("create controls failed at export arrow");
        return;
    }
    if (!create_hit(ops_btn_rect(1), 401, 0, root, &g_ops_btns[1])) {
        SB_UI_LOG("create controls failed at delete hit button");
        return;
    }
    const contracts::UiArrowButtonDesc import_arrow{ops_btn_rect(2), contracts::kUiArrowLeft, 1,
        &component_action_clicked, reinterpret_cast<void*>(402)};
    if (!contracts::succeeded(g_ui_components->arrow_button_create(&import_arrow, root, &g_ops_btns[2]))) {
        SB_UI_LOG("create controls failed at import arrow");
        return;
    }
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) {
        UiRect row = list_btn_rect(i);
        if (row.w <= 0 || row.h <= 0) row = {LIST_COL_X, 0, 1, 1};
        if (!create_hit(row, 200, i, root, &g_list_btns[i])) {
            SB_UI_LOG("create controls failed at backup row %d", i);
            return;
        }
    }
    if (!create_hit(auto_btn_rect(), 300, 0, root, &g_auto_btn)) {
        SB_UI_LOG("create controls failed at auto entry");
        return;
    }
    const contracts::UiArrowButtonDesc left{{LIST_COL_X + 0x04, PAGE_ROW_Y, LIST_PAGE_BTN_W, LIST_PAGE_BTN_H},
        contracts::kUiArrowLeft, 1, &component_action_clicked, reinterpret_cast<void*>(500)};
    const contracts::UiArrowButtonDesc right{{LIST_COL_X + LIST_COL_W - LIST_PAGE_BTN_W - 0x04,
        PAGE_ROW_Y, LIST_PAGE_BTN_W, LIST_PAGE_BTN_H}, contracts::kUiArrowRight, 1,
        &component_action_clicked, reinterpret_cast<void*>(501)};
    if (!contracts::succeeded(g_ui_components->arrow_button_create(&left, root, &g_prev_btn)) ||
        !contracts::succeeded(g_ui_components->arrow_button_create(&right, root, &g_next_btn))) {
        SB_UI_LOG("create controls failed at pager arrows");
        return;
    }
    UiRect root_abs{};
    if (!ui_get_rect(root, &root_abs)) {
        SB_UI_LOG("create controls failed at navigation root geometry");
        return;
    }
    const UiRect back{contracts::ui_theme::kNavBoxAbsX - root_abs.x,
                      contracts::ui_theme::kNavBoxAbsY - root_abs.y,
                      contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH};
    const contracts::UiNavButtonDesc nav{back, contracts::kUiNavButtonAuto, nullptr, nullptr};
    if (!contracts::succeeded(g_ui_components->nav_button_create(&nav, root, &g_back_btn))) {
        SB_UI_LOG("create controls failed at navigation button");
        return;
    }
    sync_list_btn_rects_locked();
    SB_UI_LOG("create component tree: root=%p", root);
}

void savebackup_panel_enter() {
    const auto t0 = std::chrono::steady_clock::now();
    SB_UI_LOG("savebackup panel enter");
    // 槽位刷新必须在游戏线程（enter 回调即游戏线程）执行：SAVE_CreateSaveSlot 走原版保存链，
    // 后台线程调用会崩溃（见 refresh_slots_on_game_thread 注释）。
    refresh_slots_on_game_thread();
    if (const auto* ui = ui_adapter(); ui != nullptr && ui->load_image_unit != nullptr) {
        (void)ui->load_image_unit(0x4f);
        (void)ui->load_image_unit(0x50);
    }
    bool resumed = false;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_root != nullptr) {
            // 组件树仍在时仅恢复活动标志。
            g_panel_active = true;
            g_close_delay_frames = 0;
            // 页码/条目缓存可能在隐藏期间被 self_check/刷新改动，列表按钮 rect 重对齐。
            sync_list_btn_rects_locked();
            resumed = true;
        } else if (!ui_resolution_ready()) {
            SB_UI_LOG("savebackup panel enter: symbols not resolved");
            return;
        } else {
            create_panel_controls_locked();
            if (g_root == nullptr || g_back_btn == nullptr || g_auto_btn == nullptr ||
                g_prev_btn == nullptr || g_next_btn == nullptr) {
                destroy_panel_controls_locked();
                return;
            }
            g_panel_active = true;
            g_close_delay_frames = 0;
        }
    }
    // 控件先创建并激活，再枚举文件名与 stat 元数据；bundle 内容留到逐帧队列解析。
    refresh_panel_data();
    g_skip_parse_frame = true;
    const auto t1 = std::chrono::steady_clock::now();
    SB_UI_LOG("savebackup panel enter %s in %.1f ms",
              resumed ? "resumed" : "controls created",
              std::chrono::duration<double, std::milli>(t1 - t0).count());
}

void savebackup_panel_process() {
    if (!g_panel_active) return;
    if (g_skip_parse_frame) g_skip_parse_frame = false;
    else process_one_parse_task();
    bool dialog_pending = false;
    uint64_t confirmation_token = 0;
    const contracts::UiConfirmationApi* confirmation = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        dialog_pending = g_dialog != DialogKind::kNone;
        confirmation_token = g_confirmation_token;
        confirmation = g_ui_confirmation_api;
    }
    // 原生确认框被外部关闭（未走本面板回调）时清掉 pending，避免面板停在“确认中”。
    if (dialog_pending && confirmation_token != 0 && confirmation != nullptr &&
        confirmation->active != nullptr) {
        uint8_t active = 0;
        if (!contracts::succeeded(confirmation->active(confirmation_token, &active)) || active == 0) {
            std::lock_guard<std::mutex> lock(g_sbui_mtx);
            if (g_dialog != DialogKind::kNone) clear_pending_locked();
        }
    }
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    if (g_root == nullptr) return;
    if (g_close_delay_frames > 0) {
        --g_close_delay_frames;
        if (g_close_delay_frames == 0) {
            (void)ui_set_popup_process_info(3, 0);
            return;
        }
    }
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui == nullptr || ui->begin_frame == nullptr || ui->end_frame == nullptr) return;
    // 控件 rect 写回与逐帧解析都只在游戏线程执行。
    // 列表条目数/页码变化后在此把最新 rect 写回控件，使引擎侧的控件 rect 与计算 rect 一致
    // （命中/绘制另用 panel_abs_origin + 计算 rect，双保险）。
    sync_list_btn_rects_locked();
    // 帧封装与 core.ui settings_panel_process 同款（ui_begin_frame 内含 SaveLCD/RestoreLCD）
    ui_begin_frame();
    root_decor_draw();
    sync_component_enabled_locked();
    for (int i = 0; i < 3; ++i) if (g_slot_btns[i]) slot_btn_draw(g_slot_btns[i]);
    for (int i = 0; i < 3; ++i) if (g_ops_btns[i]) ops_btn_draw(g_ops_btns[i]);
    for (int i = 0; i < LIST_COLS_PER_PAGE; ++i) if (g_list_btns[i]) list_btn_draw(g_list_btns[i]);
    if (g_auto_btn) auto_btn_draw(g_auto_btn);
    if (g_prev_btn) page_btn_draw(g_prev_btn);
    if (g_next_btn) page_btn_draw(g_next_btn);
    if (g_back_btn && g_ui_components != nullptr) (void)g_ui_components->nav_button_draw(g_back_btn);
    if (!g_msg_text.empty()) msg_label_draw(g_root);
    ui_end_frame();
}

void savebackup_panel_f3() {
    SB_UI_LOG("savebackup panel f3 (terminate)");
    close_active_confirmation();
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    g_panel_active = false;
    g_close_delay_frames = 0;
    destroy_panel_controls_locked();
    // 组件树销毁归 core.custom_ui 管理；共享图集仍由宿主/其他 UI 持有。
}

void savebackup_panel_f4() {}

uint32_t savebackup_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!g_panel_active || g_root == nullptr || g_close_delay_frames > 0 ||
        g_ui_components == nullptr || g_ui_components->dispatch_native_event == nullptr) return 1;
    if (event == 0x19) {
        uint32_t consumed = 0;
        (void)g_ui_components->dispatch_native_event(g_root, event, param, param2, &consumed);
        return consumed;
    }
    if ((event != 0x17 && event != 0x18) || param == 0) return 1;
    const auto* ui = ui_adapter();
    if (ui == nullptr || ui->touch_coordinates == nullptr || ui->hit_test == nullptr) return 1;
    int64_t x = 0, y = 0;
    if (!contracts::succeeded(ui->touch_coordinates(param, &x, &y))) return 1;
    const bool inside_back = g_back_btn != nullptr &&
        ui->hit_test(g_back_btn, x, y, {0, 0, contracts::ui_theme::kNavBoxW,
                                         contracts::ui_theme::kNavBoxH}) > 0;
    if (event == 0x17) g_back_pressed = inside_back;
    else if (g_back_pressed) {
        g_back_pressed = 0;
        if (!inside_back) return 1;
    }
    uint32_t consumed = 0;
    (void)g_ui_components->dispatch_native_event(g_root, event, param, param2, &consumed);
    return consumed;
}
