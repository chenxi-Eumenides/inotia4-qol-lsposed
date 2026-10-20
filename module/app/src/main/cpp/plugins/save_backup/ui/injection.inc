// 面板操作回调；文件 IO 均在游戏线程回调锁外执行。
static const char* response_error_text(const std::string& resp, char* buf, size_t cap,
                                       const char* fallback) {
    const char* prefix = "\"error\":\"";
    size_t p = resp.find(prefix);
    if (p == std::string::npos || cap == 0) return fallback;
    p += std::strlen(prefix);
    size_t q = resp.find('"', p);
    if (q == std::string::npos) return fallback;
    size_t n = std::min(cap - 1, q - p);
    memcpy(buf, resp.data() + p, n); buf[n] = 0;
    return buf;
}

static void export_selected() {
    int slot = -1;
    { std::lock_guard<std::mutex> lock(g_sbui_mtx);
      if (g_selected_slot >= 0 && g_slots[g_selected_slot].exists) slot = g_selected_slot; }
    if (slot < 0) { std::lock_guard<std::mutex> lock(g_sbui_mtx); set_msg_locked("请先选择有存档的槽位", COLOR_ERR, true); return; }
    const std::string resp = save_backup_export_json(slot);
    const bool ok = resp.find("\"ok\":true") != std::string::npos;
    { std::lock_guard<std::mutex> lock(g_sbui_mtx);
      if (ok) { g_page_index = 0; set_msg_locked("导出成功", COLOR_OK, false); }
      else { char b[96]; char msg[128]; snprintf(msg, sizeof(msg), "导出失败：%s", response_error_text(resp,b,sizeof(b),"未知错误")); set_msg_locked(msg,COLOR_ERR,true); } }
    if (ok) refresh_panel_data();
}

static void open_import_dialog() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    const BackupEntry* e = g_selected_auto ? (g_auto_backup_valid ? &g_auto_backup : nullptr)
                                             : selected_manual_backup();
    if (g_selected_slot < 0 || e == nullptr) {
        set_msg_locked("当前选择不支持该操作", COLOR_ERR, true);
        return;
    }
    g_dialog = DialogKind::kImport;
    g_dialog_checksum = e->checksum;
    g_dialog_slot = g_selected_slot;
    g_dialog_auto = g_selected_auto;
}

static void open_delete_dialog() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    const bool slot = g_selected_slot >= 0;
    const BackupEntry* e = selected_manual_backup();
    if (slot == (e != nullptr) || g_selected_auto) {
        set_msg_locked("当前选择不支持该操作", COLOR_ERR, true);
        return;
    }
    g_dialog = slot ? DialogKind::kDeleteSlot : DialogKind::kDeleteBackup;
    g_dialog_checksum = e != nullptr ? e->checksum : "";
    g_dialog_slot = slot ? g_selected_slot : -1;
}

static void execute_dialog_confirm() {
    DialogKind kind; std::string checksum; int slot; bool from_auto;
    { std::lock_guard<std::mutex> lock(g_sbui_mtx);
      kind = g_dialog; checksum = g_dialog_checksum; slot = g_dialog_slot; from_auto = g_dialog_auto;
      clear_pending_locked(); }
    if (kind == DialogKind::kNone) return;
    std::string resp;
    if (kind == DialogKind::kImport)
        resp = from_auto ? save_backup_import_auto_json(slot, slot) : save_backup_import_json(checksum.c_str(), slot);
    else if (kind == DialogKind::kDeleteSlot) resp = save_backup_delete_slot_json(slot);
    else resp = save_backup_delete_json(checksum.c_str());
    const bool ok = resp.find("\"ok\":true") != std::string::npos;
    { std::lock_guard<std::mutex> lock(g_sbui_mtx);
      if (ok) {
          set_msg_locked(kind == DialogKind::kImport ? "导入成功" : "删除成功", COLOR_OK, false);
          if (kind == DialogKind::kDeleteSlot) g_selected_slot = -1;
          if (kind == DialogKind::kDeleteBackup) {
              g_selected_backup = -1;
              g_selected_backup_name.clear();
          }
      } else {
          char b[96]; char msg[128];
          const char* action = kind == DialogKind::kImport ? "导入失败：" : "删除失败：";
          snprintf(msg,sizeof(msg),"%s%s",action,response_error_text(resp,b,sizeof(b),"未知错误"));
          set_msg_locked(msg,COLOR_ERR,true);
      } }
    if (ok) {
        if (kind == DialogKind::kImport || kind == DialogKind::kDeleteSlot) refresh_slots_on_game_thread();
        refresh_panel_data();
    }
}

void on_btn_clicked(void* ctrl) {
    if (ctrl == nullptr) return;
    int op = -1; bool prev = false, next = false, back = false;
    { std::lock_guard<std::mutex> lock(g_sbui_mtx);
      if (!g_panel_active || g_dialog != DialogKind::kNone) return;
      for (int i=0;i<3;++i) if (ctrl == g_ops_btns[i]) op=i;
      prev = ctrl == g_prev_btn; next = ctrl == g_next_btn; back = ctrl == g_back_btn; }
    if (back) { on_back_clicked(ctrl); return; }
    if (op == 0) { export_selected(); return; }
    if (op == 1) { open_delete_dialog(); return; }
    if (op == 2) { open_import_dialog(); return; }
    if (prev || next) {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        const int old_page = g_page_index;
        if (prev && g_page_index > 0) --g_page_index;
        else if (next && g_page_index < g_total_pages-1) ++g_page_index;
        if (old_page != g_page_index) rebuild_parse_queue_locked();
        sync_list_btn_rects_locked();
    }
}

void on_back_clicked(void* ctrl) {
    if (ctrl != g_back_btn) return;
    { std::lock_guard<std::mutex> lock(g_sbui_mtx);
      if (g_dialog != DialogKind::kNone) { clear_pending_locked(); return; } }
    savebackup_provider_close();
}
