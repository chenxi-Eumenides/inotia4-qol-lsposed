// PopupState 注入 / 还原 + 懒注入线程

uint8_t* find_state_entry(uintptr_t enter_vma) {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || *got == nullptr) return nullptr;
    uint8_t* list = reinterpret_cast<uint8_t*>(*got);
    for (int i = 0; i < 27; ++i) {
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * POPUP_STATE_SIZE + POPUP_ENTRY_ENTER);
        if (enter == g_base + enter_vma) return list + i * POPUP_STATE_SIZE;
    }
    return nullptr;
}

bool inject_state_entry_locked() {
    if (g_state_entry != nullptr) return true;
    if (g_base == 0) return false;
    uint8_t* entry = find_state_entry(F_PANEL_UNK2_ENTER);  // INAP_GOODS（IAP 屏蔽=死条目）
    if (entry == nullptr) {
        SB_UI_LOG("state entry (INAP_GOODS) not found");
        return false;
    }
    memcpy(g_state_backup, entry, POPUP_STATE_SIZE);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER) =
        reinterpret_cast<uintptr_t>(&savebackup_panel_enter);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_PROCESS) =
        reinterpret_cast<uintptr_t>(&savebackup_panel_process);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_F3) =
        reinterpret_cast<uintptr_t>(&savebackup_panel_f3);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_F4) =
        reinterpret_cast<uintptr_t>(&savebackup_panel_f4);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_EVENT) =
        reinterpret_cast<uintptr_t>(&savebackup_panel_event);
    g_state_entry = entry;
    g_state_id = *reinterpret_cast<int32_t*>(entry);
    SB_UI_LOG("state entry %d injected (INAP_GOODS, enter/process/f3/f4/event)",
              g_state_id);
    return true;
}

void restore_state_entry_locked() {
    if (g_state_entry == nullptr) return;
    memcpy(g_state_entry, g_state_backup, POPUP_STATE_SIZE);
    SB_UI_LOG("state entry %d restored from backup", g_state_id);
    g_state_entry = nullptr;
    g_state_id = -1;
}

void ensure_inject_thread() {
    if (g_thread_started.exchange(true)) return;
    std::thread([]() {
        for (;;) {
            const char* screen = data_ui_screen();
            const bool on_main_menu = screen != nullptr &&
                                      (strncmp(screen, "main_menu", 9) == 0);
            {
                std::lock_guard<std::mutex> lock(g_sbui_mtx);
                if (on_main_menu) {
                    // 进入主菜单：若已被还原（离开过主菜单）则重新注入。
                    if (g_state_entry == nullptr) {
                        inject_state_entry_locked();
                    }
                } else {
                    // 离开主菜单：还原原始回调，不永久占用 IAP 死条目
                    // （与 game_ui_settings 同设计：避免 27 条状态被长期拦截影响游戏内 IAP 检测路径）。
                    if (g_state_entry != nullptr) {
                        restore_state_entry_locked();
                        g_panel_active = false;
                    }
                }
            }
            // 锁必须在 sleep 前释放：否则每轮持锁睡 300ms，会饿死其它 g_sbui_mtx 持有者
            // （self_check / 面板 enter 曾因此卡死或延迟数秒）。
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

// ============== 操作回调（按钮点击）==============
//
// 两步确认（面板内，不依赖原版 YesNo 弹窗——自定义面板下该弹窗不渲染）：
// 首次点击武装 g_pending_op + checksum + 槽并提示；再次点击同操作且 checksum/槽一致才执行。
// 任何其它交互（槽位/选中切换、翻页、导出）清除待确认，见 panel.inc 事件分发与本文件各回调。
// 契约：本文件所有操作回调均由 on_btn_clicked 在「不持 g_sbui_mtx」下调用；
// 需要读/写共享状态的片段各自短临界区拿锁，文件 IO 一律在锁外。

static void export_selected() {
    int slot = 0;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        // 导出属于「其它操作」：点击即取消待确认（并清提示，避免残留确认文案）。
        if (g_pending_op != 0) {
            clear_pending_locked();
            set_msg_locked("", UI_OPTION_TEXT, false);
        }
        // 未选中槽（再次点击已选槽取消）→ 不可用态下的点击反馈。
        if (g_selected_slot < 0) {
            set_msg_locked("请先选中一个存档槽", COLOR_ERR, true);
            return;
        }
        slot = g_selected_slot;
    }
    SB_UI_LOG("export: slot=%d", slot);
    // 导出（目录 IO + 加密）在锁外执行，避免持 g_sbui_mtx 做长 IO。
    const std::string resp = save_backup_export_json(slot);
    if (resp.find("\"ok\":true") != std::string::npos) {
        {
            std::lock_guard<std::mutex> lock(g_sbui_mtx);
            set_msg_locked("导出成功", COLOR_OK, false);
            // 新备份按导出时间倒序位于第 0 页（列表顶部）。apply_backups_locked 只对
            // g_page_index 做上限钳制、不回第 1 页：备份多于 1 页（>LIST_COLS_PER_PAGE 个）
            // 且用户停在第 2+ 页导出时，刷新后仍停在原页 → 看不到新条目
            // （用户反馈「创建备份后看不到」的成因）。导出成功一律回第 1 页。
            g_page_index = 0;
        }
        refresh_panel_data_async();
    } else {
        // 抽取 error 字符串
        size_t p = resp.find("\"error\":\"");
        char buf[64];
        if (p != std::string::npos) {
            p += std::string("\"error\":\"").size();
            size_t q = resp.find('"', p);
            if (q != std::string::npos) {
                size_t n = q - p < sizeof(buf) - 1 ? q - p : sizeof(buf) - 1;
                memcpy(buf, resp.c_str() + p, n);
                buf[n] = 0;
            } else {
                snprintf(buf, sizeof(buf), "导出失败");
            }
        } else {
            snprintf(buf, sizeof(buf), "导出失败");
        }
        char full[80];
        snprintf(full, sizeof(full), "导出失败: %s", buf);
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        set_msg_locked(full, COLOR_ERR, true);
    }
}

static void import_selected() {
    std::string checksum;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_selected_backup < 0) {
            set_msg_locked("请先选中一个备份", COLOR_ERR, true);
            return;
        }
        // 导入需目标槽：未选中槽（再次点击已选槽取消）→ 不可用态下的点击反馈。
        if (g_selected_slot < 0) {
            set_msg_locked("请先选中一个存档槽", COLOR_ERR, true);
            return;
        }
        const BackupEntry* e = backup_at_page(g_selected_backup);
        if (e == nullptr) {
            set_msg_locked("备份项无效", COLOR_ERR, true);
            return;
        }
        // 待确认与 checksum + 目标槽绑定：同 checksum + 同槽才算第二次确认；
        // 否则（无待确认/选中或槽已变）按新状态重新武装。
        const bool armed = (g_pending_op == 1 &&
                            g_pending_checksum == e->checksum &&
                            g_pending_slot == g_selected_slot);
        if (!armed) {
            g_pending_op = 1;
            g_pending_checksum = e->checksum;
            g_pending_slot = g_selected_slot;
            char buf[64];
            snprintf(buf, sizeof(buf), "再次点击「← 导入」确认导入到 槽%d", g_selected_slot);
            set_msg_locked(buf, COLOR_INFO, false);
            SB_UI_LOG("import armed: slot=%d checksum=%.8s", g_selected_slot, e->checksum.c_str());
            return;
        }
        // 确认：先清待确认并快照参数，锁外执行导入。
        clear_pending_locked();
        checksum = e->checksum;
        slot = g_selected_slot;
    }
    // 导入（文件 IO + 解密）在锁外执行，避免持 g_sbui_mtx 做长 IO。
    const std::string resp = save_backup_import_json(checksum.c_str(), slot);
    const bool ok = resp.find("\"ok\":true") != std::string::npos;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        set_msg_locked(ok ? "导入成功" : "导入失败", ok ? COLOR_OK : COLOR_ERR, !ok);
    }
    if (ok) {
        // 导入改变目标槽文件 → 槽结构必须在游戏线程重载（本回调即游戏线程），
        // 且 refresh_slots_on_game_thread 内部自加 g_sbui_mtx，必须在锁外调用。
        refresh_slots_on_game_thread();
        refresh_panel_data_async();
    }
}

static void delete_selected() {
    // 删除目标二选一（恰好选中一侧）：备份 → checksum；存档槽 → slot。
    std::string checksum;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        const bool slot_sel = g_selected_slot >= 0;
        const bool backup_sel =
            g_selected_backup >= 0 && backup_at_page(g_selected_backup) != nullptr;
        if (slot_sel == backup_sel) {
            // 两侧都选/都不选：删除不可用（渲染层已置灰），点击给出反馈。
            set_msg_locked("请只选中存档槽或备份之一", COLOR_ERR, true);
            return;
        }
        if (backup_sel) {
            const BackupEntry* e = backup_at_page(g_selected_backup);
            if (e == nullptr) {
                set_msg_locked("备份项无效", COLOR_ERR, true);
                return;
            }
            // 两步确认：同 checksum 才执行（op=2）。
            const bool armed = (g_pending_op == 2 && g_pending_checksum == e->checksum);
            if (!armed) {
                g_pending_op = 2;
                g_pending_checksum = e->checksum;
                g_pending_slot = -1;
                set_msg_locked("再次点击「删除」确认删除该备份", COLOR_INFO, false);
                SB_UI_LOG("delete backup armed: checksum=%.8s", e->checksum.c_str());
                return;
            }
            clear_pending_locked();
            checksum = e->checksum;
        } else {
            // 删该槽游戏存档；两步确认绑定 slot（op=3），与备份删除区分。
            slot = g_selected_slot;
            const bool armed = (g_pending_op == 3 && g_pending_slot == slot);
            if (!armed) {
                g_pending_op = 3;
                g_pending_checksum.clear();
                g_pending_slot = slot;
                char buf[96];
                snprintf(buf, sizeof(buf), "再次点击「删除」确认删除 槽%d 的存档（不可恢复）",
                         slot);
                set_msg_locked(buf, COLOR_INFO, false);
                SB_UI_LOG("delete slot armed: slot=%d", slot);
                return;
            }
            clear_pending_locked();
        }
    }
    // 删除（文件 IO）在锁外执行。
    const std::string resp = !checksum.empty() ? save_backup_delete_json(checksum.c_str())
                                               : save_backup_delete_slot_json(slot);
    const bool ok = resp.find("\"ok\":true") != std::string::npos;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (ok) {
            set_msg_locked(checksum.empty() ? "已删除存档" : "已删除", COLOR_OK, false);
            if (checksum.empty()) g_selected_slot = -1;
            else g_selected_backup = -1;
        } else {
            set_msg_locked("删除失败", COLOR_ERR, true);
        }
    }
    if (ok) {
        // 删槽改动了游戏存档文件 → 槽结构必须在游戏线程重载（本回调即游戏线程，锁外调用）。
        if (checksum.empty()) refresh_slots_on_game_thread();
        refresh_panel_data_async();  // 备份列表刷新（删槽不改 bundle，但代价低且保持缓存新鲜）
    }
}

// 控件通用点击回调（kind 通过 ctrl 指针反查）。
// 契约：调用方不得持 g_sbui_mtx（导入/删除确认路径需锁外 IO 与锁外 refresh_slots）。
void on_btn_clicked(void* ctrl) {
    if (ctrl == nullptr) return;
    int op_index = -1;
    bool is_prev = false;
    bool is_next = false;
    bool is_back = false;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (!g_panel_active) return;
        for (int i = 0; i < 3; ++i) {
            if (ctrl == g_ops_btns[i]) { op_index = i; break; }
        }
        if (op_index < 0) {
            is_prev = (ctrl == g_prev_btn);
            is_next = (ctrl == g_next_btn);
            is_back = (ctrl == g_back_btn);
        }
    }
    if (is_back) { on_back_clicked(ctrl); return; }
    // 操作按钮（导出/导入/删除）：各自内部管理锁与 IO。
    if (op_index == 0) { export_selected(); return; }
    if (op_index == 1) { import_selected(); return; }
    if (op_index == 2) { delete_selected(); return; }
    // 翻页：翻页属于「其它交互」，点击即清除待确认；有效翻页仍按原行为清提示。
    if (is_prev || is_next) {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_pending_op != 0) {
            clear_pending_locked();
            set_msg_locked("", UI_OPTION_TEXT, false);
        }
        if (is_prev && g_page_index > 0) {
            --g_page_index;
            g_selected_backup = -1;
            set_msg_locked("", UI_OPTION_TEXT, false);
        } else if (is_next && g_page_index < g_total_pages - 1) {
            ++g_page_index;
            g_selected_backup = -1;
            set_msg_locked("", UI_OPTION_TEXT, false);
        } else {
            return;
        }
        // 页码变化 → 本页条目数可能变化：垂直居中分布的列表按钮 rect 重对齐。
        sync_list_btn_rects_locked();
    }
    // 槽位 / 备份项不在 ExecuteProc 走：命中测试已在 event 中处理 selection；此处留空
    // 以防 ui_create_button 自动调 click（实测：ui_create_button 不会自动调 ExecuteProc）
}

// 「← 返回」点击：清待确认/提示 → 停用面板 → 关闭当前 PopupState。
// 契约：调用方（savebackup_panel_event 命中测试）不持 g_sbui_mtx。
// 关闭走 fn_ui_set_popup_process_info(3, 0)（push state 0 + clear draw flag，官方 close
// panel 路径，data_savebackup_ui_open_panel 切换式兜底同款）：无独立 pop 符号，push 模式
// 下弹出本层回设置页，切换式下回主菜单。游戏消费 (3,0) pop 时会调 savebackup_panel_f3
// 再次清状态（幂等）；本函数先置 g_panel_active=false，使 (3,0) 生效前的帧立即停止
// event/process 处理。fn_ui_set_popup_process_info 必须锁外调用：pop 链会触发
// savebackup_panel_f3 拿 g_sbui_mtx（非递归），持锁调用会死锁。
void on_back_clicked(void* ctrl) {
    if (ctrl == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (!g_panel_active || ctrl != g_back_btn) return;
        clear_pending_locked();
        set_msg_locked("", UI_OPTION_TEXT, false);
        g_panel_active = false;
        g_close_delay_frames = 0;
        g_back_pressed = 0;
        // 控件树保留复用（同 f3：递归删除 SIGBUS）；再次进入走 enter 的 resumed 分支。
    }
    if (fn_ui_set_popup_process_info != nullptr) {
        fn_ui_set_popup_process_info(3, 0);
    }
    SB_UI_LOG("back clicked: panel closed");
}
