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

static bool open_confirmation(const std::string& message);
static void reject_confirmation_open();

static void execute_export(int slot) {
    const std::string response = save_backup_export_json(slot);
    const bool ok = response.find("\"ok\":true") != std::string::npos;
    if (ok) {
        {
            std::lock_guard<std::mutex> lock(g_sbui_mtx);
            g_page_index = 0;
        }
        SB_UI_LOG("export succeeded: slot=%d", slot);
        refresh_panel_data();
    } else {
        char error[96];
        SB_UI_LOG("export failed: slot=%d error=%s", slot,
                  response_error_text(response, error, sizeof(error), "未知错误"));
    }
}

static void open_export_dialog() {
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_selected_slot >= 0 && g_slots[g_selected_slot].exists) slot = g_selected_slot;
    }
    if (slot < 0) {
        SB_UI_LOG("export unavailable: no occupied slot selected");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        g_dialog = DialogKind::kExport;
        g_dialog_slot = slot;
    }
    const std::string prompt = "导出槽" + std::to_string(slot + 1) + "的存档为备份？";
    if (!open_confirmation(prompt)) reject_confirmation_open();
}

static void execute_dialog_confirm();

// 确认框回调由 core.custom_ui 在游戏主线程派发，派发前 core 已清掉自身状态；
// 这里释放 token 后推进业务（确认）或仅清理 pending（取消）。
static void on_confirmation_confirm(void*) {
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        g_confirmation_token = 0;
    }
    execute_dialog_confirm();
}

static void on_confirmation_cancel(void*) {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    g_confirmation_token = 0;
    clear_pending_locked();
}

static bool open_confirmation(const std::string& message) {
    const contracts::UiConfirmationApi* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        api = g_ui_confirmation_api;
    }
    if (api == nullptr || api->open_text == nullptr) return false;
    contracts::UiConfirmationSpecText spec{};
    spec.size = static_cast<uint32_t>(sizeof(spec));
    const size_t message_size = std::min(message.size(), sizeof(spec.message) - 1);
    std::memcpy(spec.message, message.data(), message_size);
    spec.message[message_size] = '\0';
    spec.price = 0;
    spec.on_confirm = &on_confirmation_confirm;
    spec.on_cancel = &on_confirmation_cancel;
    spec.user = nullptr;
    uint64_t token = 0;
    if (contracts::succeeded(api->open_text(&spec, &token))) {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        g_confirmation_token = token;
        return true;
    }
    // 打开失败时契约允许保留 token 供调用方关闭，避免 core 侧残留 owner 状态。
    if (token != 0 && api->close != nullptr) (void)api->close(token);
    return false;
}

static void reject_confirmation_open() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    clear_pending_locked();
    SB_UI_LOG("confirmation open failed");
}

static void open_import_dialog() {
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        const BackupEntry* entry = g_selected_auto ? (g_auto_backup_valid ? &g_auto_backup : nullptr)
                                                   : selected_manual_backup();
        if (g_selected_slot < 0 || entry == nullptr) {
            SB_UI_LOG("import unavailable: selected slot or backup is invalid");
            return;
        }
        g_dialog = DialogKind::kImport;
        g_dialog_checksum = entry->checksum;
        g_dialog_slot = g_selected_slot;
        g_dialog_auto = g_selected_auto;
        const bool occupied = g_slots[g_dialog_slot].exists;
        g_confirmation_prompt = "导入备份到槽" + std::to_string(g_dialog_slot + 1) +
                                (occupied ? "？将覆盖现有存档。" : "？");
    }
    std::string prompt;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        prompt = g_confirmation_prompt;
    }
    if (!open_confirmation(prompt)) reject_confirmation_open();
}

static void open_delete_dialog() {
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        const bool slot_selected = g_selected_slot >= 0;
        const BackupEntry* entry = selected_manual_backup();
        if (slot_selected == (entry != nullptr) || g_selected_auto) {
            SB_UI_LOG("delete unavailable: selection is invalid");
            return;
        }
        g_dialog = slot_selected ? DialogKind::kDeleteSlot : DialogKind::kDeleteBackup;
        g_dialog_checksum = entry != nullptr ? entry->checksum : "";
        g_dialog_slot = slot_selected ? g_selected_slot : -1;
        if (slot_selected) {
            g_confirmation_prompt =
                "删除槽" + std::to_string(g_selected_slot + 1) + "的存档？不可恢复。";
        } else {
            g_confirmation_prompt = "删除选中的备份？";
        }
    }
    std::string prompt;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        prompt = g_confirmation_prompt;
    }
    if (!open_confirmation(prompt)) reject_confirmation_open();
}

// 屏幕最右列「导出到 Download」：把选中的手动/自动备份源文件路径交给 Host 公共导出入口
// （异步复制到 Download/qol/backups/，结果由 Kotlin 写日志）。只入队，不改面板数据。
static void open_export_download_dialog() {
    const auto* api = contracts::public_export_if_available(save_backup_host_sdk());
    if (api == nullptr) {
        SB_UI_LOG("export to download unavailable: public_export api not ready");
        return;
    }
    std::string path;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        const BackupEntry* manual = selected_manual_backup();
        if (manual != nullptr) {
            std::string dir;
            if (!::save_backup::persistence::backup_dir(
                    ::save_backup::state::snapshot().external_dir, dir)) {
                SB_UI_LOG("export to download unavailable: backup dir unresolved");
                return;
            }
            path = dir + "/" + manual->name;
            name = manual->name;
        } else if (g_selected_auto && g_auto_backup_valid) {
            const std::optional<std::string> auto_path = ::save_backup::persistence::auto_backup_path(
                ::save_backup::state::snapshot().external_dir, g_selected_slot);
            if (!auto_path.has_value()) {
                SB_UI_LOG("export to download unavailable: auto backup file missing");
                return;
            }
            path = *auto_path;
            name = g_auto_backup.name;
        } else {
            SB_UI_LOG("export to download unavailable: no backup selected");
            return;
        }
        g_dialog = DialogKind::kExportDownload;
        g_dialog_path = path;
        g_dialog_name = name;
    }
    if (!open_confirmation("将 " + name + " 复制到 Download/qol/backups/？")) {
        reject_confirmation_open();
    }
}

// 屏幕最右列「从文件导入」：拉起系统文件选择器（SAF）；选中文件由 Host 归入 qol/backups/
// 后刷新存档面板。不要求面板内有选中项。
static void pick_import_file_via_host() {
    const auto* api = contracts::public_export_if_available(save_backup_host_sdk());
    if (api == nullptr) {
        SB_UI_LOG("import from file unavailable: public_export api not ready");
        return;
    }
    if (!contracts::succeeded(api->pick_import_file())) {
        SB_UI_LOG("import from file failed: picker unavailable");
    }
}

static void execute_dialog_confirm() {
    DialogKind kind;
    std::string checksum;
    std::string path;
    std::string name;
    int slot;
    bool from_auto;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        kind = g_dialog;
        checksum = g_dialog_checksum;
        slot = g_dialog_slot;
        from_auto = g_dialog_auto;
        path = g_dialog_path;
        name = g_dialog_name;
        clear_pending_locked();
    }
    if (kind == DialogKind::kNone) return;
    if (kind == DialogKind::kExport) {
        execute_export(slot);
        return;
    }
    if (kind == DialogKind::kExportDownload) {
        // Host 侧异步复制：调用只入队、立即返回，完成结果由 Kotlin 写日志；
        // Download 副本不在本面板列表内，无需刷新面板数据。
        const auto* api = contracts::public_export_if_available(save_backup_host_sdk());
        if (api == nullptr || !contracts::succeeded(api->export_backup(path.c_str(), name.c_str()))) {
            SB_UI_LOG("export to download failed: %s", name.c_str());
        } else {
            SB_UI_LOG("export to download queued: %s", name.c_str());
        }
        return;
    }
    std::string response;
    if (kind == DialogKind::kImport)
        response = from_auto ? save_backup_import_auto_json(slot, slot)
                             : save_backup_import_json(checksum.c_str(), slot);
    else if (kind == DialogKind::kDeleteSlot)
        response = save_backup_delete_slot_json(slot);
    else
        response = save_backup_delete_json(checksum.c_str());
    const bool ok = response.find("\"ok\":true") != std::string::npos;
    if (ok) {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (kind == DialogKind::kDeleteSlot) g_selected_slot = -1;
        if (kind == DialogKind::kDeleteBackup) {
            g_selected_backup = -1;
            g_selected_backup_name.clear();
        }
        SB_UI_LOG("%s succeeded", kind == DialogKind::kImport ? "import" : "delete");
    } else {
        char error[96];
        SB_UI_LOG("%s failed: %s", kind == DialogKind::kImport ? "import" : "delete",
                  response_error_text(response, error, sizeof(error), "未知错误"));
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
        if (op == 0) open_export_dialog();
        else if (op == 1) open_delete_dialog();
        else open_import_dialog();
    } else if (action == 403 || action == 404) {
        {
            std::lock_guard<std::mutex> lock(g_sbui_mtx);
            if (g_dialog != DialogKind::kNone) return;
            // 纯命中按钮没有引擎 Enabled 位：点击时按绘制同口径再判一次可用性。
            if (!side_btn_enabled(action == 403 ? 0 : 1)) return;
        }
        if (action == 403) open_export_download_dialog();
        else pick_import_file_via_host();
    } else if (action == 500 || action == 501) {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_dialog != DialogKind::kNone) return;
        const int previous_page = g_page_index;
        if (action == 500 && g_page_index > 0) --g_page_index;
        else if (action == 501 && g_page_index < g_total_pages - 1) ++g_page_index;
        if (g_page_index != previous_page) {
            g_selected_backup = -1;
            g_selected_backup_name.clear();
        }
        rebuild_parse_queue_locked();
    }
}

void close_active_confirmation() {
    const contracts::UiConfirmationApi* api = nullptr;
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        api = g_ui_confirmation_api;
        token = g_confirmation_token;
        g_confirmation_token = 0;
    }
    // close 走游戏 UI 栈，可能同步触发回调并取 g_sbui_mtx，必须锁外调用。
    if (token != 0 && api != nullptr && api->close != nullptr) (void)api->close(token);
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    if (g_dialog != DialogKind::kNone) clear_pending_locked();
}
