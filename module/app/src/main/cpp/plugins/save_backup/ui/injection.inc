// Component callbacks own input dispatch; long-running business calls remain outside the UI lock.
static const char* response_error_text(const std::string& resp, char* buf, size_t cap,
                                       const char* fallback) {
    const char* prefix = "\"error\":\"";
    size_t p = resp.find(prefix);
    if (p == std::string::npos || cap == 0) return fallback;
    p += std::strlen(prefix);
    size_t q = resp.find('"', p);
    if (q == std::string::npos) return fallback;
    const size_t n = std::min(cap - 1, q - p);
    memcpy(buf, resp.data() + p, n);
    buf[n] = 0;
    return buf;
}

static void export_selected() {
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_selected_slot >= 0 && g_slots[g_selected_slot].exists) slot = g_selected_slot;
    }
    if (slot < 0) {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        set_msg_locked("请先选择有存档的槽位", COLOR_ERR, true);
        return;
    }
    const std::string response = save_backup_export_json(slot);
    const bool ok = response.find("\"ok\":true") != std::string::npos;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (ok) {
            g_page_index = 0;
            set_msg_locked("导出成功", COLOR_OK, false);
        } else {
            char error[96], message[128];
            snprintf(message, sizeof(message), "导出失败：%s",
                     response_error_text(response, error, sizeof(error), "未知错误"));
            set_msg_locked(message, COLOR_ERR, true);
        }
    }
    if (ok) refresh_panel_data();
}

static void execute_dialog_confirm();

static void on_native_choice_selected(int index, void*) {
    if (index == 0) execute_dialog_confirm();
}

static bool open_native_choice(const std::string& prompt) {
    if (g_ui_choice_api == nullptr || g_ui_choice_api->open == nullptr) return false;
    static const char* const items[] = {"确认", "取消"};
    const contracts::UiChoiceSpec spec{items, 2, prompt.c_str(), 1,
                                       &on_native_choice_selected, nullptr,
                                       "save_backup.confirm"};
    return g_ui_choice_api->open(spec);
}

static void reject_choice_open() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    clear_pending_locked();
    set_msg_locked("已有原生选择框打开", COLOR_ERR, true);
}

static void open_import_dialog() {
    std::string prompt;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        const BackupEntry* entry = g_selected_auto ? (g_auto_backup_valid ? &g_auto_backup : nullptr)
                                                   : selected_manual_backup();
        if (g_selected_slot < 0 || entry == nullptr) {
            set_msg_locked("当前选择不支持该操作", COLOR_ERR, true);
            return;
        }
        g_dialog = DialogKind::kImport;
        g_dialog_checksum = entry->checksum;
        g_dialog_slot = g_selected_slot;
        g_dialog_auto = g_selected_auto;
        prompt = "导入备份到槽" + std::to_string(g_dialog_slot + 1) + "？";
        if (g_slots[g_dialog_slot].exists) prompt += "将覆盖现有存档。";
    }
    if (!open_native_choice(prompt)) reject_choice_open();
}

static void open_delete_dialog() {
    std::string prompt;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        const bool slot_selected = g_selected_slot >= 0;
        const BackupEntry* entry = selected_manual_backup();
        if (slot_selected == (entry != nullptr) || g_selected_auto) {
            set_msg_locked("当前选择不支持该操作", COLOR_ERR, true);
            return;
        }
        g_dialog = slot_selected ? DialogKind::kDeleteSlot : DialogKind::kDeleteBackup;
        g_dialog_checksum = entry != nullptr ? entry->checksum : "";
        g_dialog_slot = slot_selected ? g_selected_slot : -1;
        prompt = slot_selected
            ? "删除 槽" + std::to_string(g_dialog_slot + 1) + " 的存档？不可恢复。"
            : "删除选中的备份？";
    }
    if (!open_native_choice(prompt)) reject_choice_open();
}

static void execute_dialog_confirm() {
    DialogKind kind;
    std::string checksum;
    int slot;
    bool from_auto;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        kind = g_dialog;
        checksum = g_dialog_checksum;
        slot = g_dialog_slot;
        from_auto = g_dialog_auto;
        clear_pending_locked();
    }
    if (kind == DialogKind::kNone) return;
    std::string response;
    if (kind == DialogKind::kImport)
        response = from_auto ? save_backup_import_auto_json(slot, slot)
                             : save_backup_import_json(checksum.c_str(), slot);
    else if (kind == DialogKind::kDeleteSlot)
        response = save_backup_delete_slot_json(slot);
    else
        response = save_backup_delete_json(checksum.c_str());
    const bool ok = response.find("\"ok\":true") != std::string::npos;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (ok) {
            set_msg_locked(kind == DialogKind::kImport ? "导入成功" : "删除成功", COLOR_OK, false);
            if (kind == DialogKind::kDeleteSlot) g_selected_slot = -1;
            if (kind == DialogKind::kDeleteBackup) {
                g_selected_backup = -1;
                g_selected_backup_name.clear();
            }
        } else {
            char error[96], message[128];
            const char* prefix = kind == DialogKind::kImport ? "导入失败：" : "删除失败：";
            snprintf(message, sizeof(message), "%s%s", prefix,
                     response_error_text(response, error, sizeof(error), "未知错误"));
            set_msg_locked(message, COLOR_ERR, true);
        }
    }
    if (ok) {
        if (kind == DialogKind::kImport || kind == DialogKind::kDeleteSlot)
            refresh_slots_on_game_thread();
        refresh_panel_data();
    }
}

void component_action_clicked(void* user) {
    const intptr_t action = reinterpret_cast<intptr_t>(user);
    if (action >= 100 && action < 103) {
        const int slot = static_cast<int>(action - 100);
        {
            std::lock_guard<std::mutex> lock(g_sbui_mtx);
            if (g_dialog != DialogKind::kNone) return;
            g_selected_slot = g_selected_slot == slot ? -1 : slot;
            g_selected_auto = false;
            g_auto_backup_valid = false;
            g_auto_loading = false;
            set_msg_locked("", UI_OPTION_TEXT, false);
        }
        refresh_panel_data();
    } else if (action >= 200 && action < 200 + LIST_COLS_PER_PAGE) {
        const int local = static_cast<int>(action - 200);
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_dialog != DialogKind::kNone) return;
        if (backup_at_page(local) == nullptr) return;
        const int absolute = g_page_index * LIST_COLS_PER_PAGE + local;
        g_selected_backup = g_selected_backup == absolute ? -1 : absolute;
        g_selected_backup_name = g_selected_backup >= 0
            ? g_backups[static_cast<size_t>(absolute)].name : "";
        g_selected_auto = false;
    } else if (action == 300) {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_dialog != DialogKind::kNone || !g_auto_backup_valid) return;
        g_selected_auto = !g_selected_auto;
        if (g_selected_auto) {
            g_selected_backup = -1;
            g_selected_backup_name.clear();
        }
    } else if (action >= 400 && action < 403) {
        {
            std::lock_guard<std::mutex> lock(g_sbui_mtx);
            if (g_dialog != DialogKind::kNone) return;
        }
        const int op = static_cast<int>(action - 400);
        if (op == 0) export_selected();
        else if (op == 1) open_delete_dialog();
        else open_import_dialog();
    } else if (action == 500 || action == 501) {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_dialog != DialogKind::kNone) return;
        if (action == 500 && g_page_index > 0) --g_page_index;
        else if (action == 501 && g_page_index < g_total_pages - 1) ++g_page_index;
        rebuild_parse_queue_locked();
    }
}

void close_active_native_choice() {
    const contracts::UiChoiceApi* api = g_ui_choice_api;
    bool owns_choice = false;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        owns_choice = g_dialog != DialogKind::kNone;
    }
    if (owns_choice && api != nullptr && api->active != nullptr && api->close != nullptr && api->active())
        api->close();
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    if (g_dialog != DialogKind::kNone) clear_pending_locked();
}
