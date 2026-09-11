void* more_games_btn() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<void**>(g_base + G_MAINMENU_BASE_VMA + G_MAINMENU_MOREGAMES_SLOT);
}

void more_games_clicked(void* ctrl) {
    SETTINGS_LOG("more games clicked -> open settings panel");
    if (fn_ui_set_popup_process_info == nullptr || g_state_id < 0) return;
    fn_ui_set_popup_process_info(1, g_state_id);
}

bool inject_more_games_btn() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_more_games_injected) return true;
    if (g_base == 0) return false;
    void* btn = more_games_btn();
    if (btn == nullptr) return false;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(btn) + CO_DATA);
    if (data == nullptr) return false;
    void** slot = reinterpret_cast<void**>(data + CB_EXECUTE_PROC);
    if (*slot == reinterpret_cast<void*>(&more_games_clicked)) {
        g_more_games_injected = true;
        return true;
    }
    if (!g_more_games_hook.install_typed(slot, &more_games_clicked)) return false;
    g_more_games_injected = true;
    SETTINGS_LOG("more games ExecuteProc hooked: ctrl=%p orig=%p", btn, g_more_games_hook.orig);
    return true;
}

void ensure_inject_thread() {
    if (g_thread_started.exchange(true)) return;
    std::thread([]() {
        for (;;) {
            const char* screen = data_ui_screen();
            bool on_main_menu = screen != nullptr && strcmp(screen, "main_menu") == 0;
            if (on_main_menu) {
                inject_state_entry_locked();
                inject_more_games_btn();
            } else if (g_more_games_injected) {
                // 离开主菜单后控件树被释放，仅重置标志，不 uninstall（避免写已释放内存）
                std::lock_guard<std::mutex> lock(g_settings_mtx);
                g_more_games_injected = false;
                g_more_games_hook.slot = nullptr;
                g_more_games_hook.orig = nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

// ---- state list 条目定位与注入 ----

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
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_state_entry != nullptr) return true;
    if (g_base == 0) return false;
    uint8_t* entry = find_state_entry(F_PANEL_UNK1_ENTER);
    if (entry == nullptr) {
        SETTINGS_LOG("state entry (INAP_GEMSHOP) not found");
        return false;
    }
    memcpy(g_state_backup, entry, POPUP_STATE_SIZE);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER) = reinterpret_cast<uintptr_t>(&settings_panel_enter);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_PROCESS) = reinterpret_cast<uintptr_t>(&settings_panel_process);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_F3) = reinterpret_cast<uintptr_t>(&settings_panel_f3);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_F4) = reinterpret_cast<uintptr_t>(&settings_panel_f4);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_EVENT) = reinterpret_cast<uintptr_t>(&settings_panel_event);
    g_state_entry = entry;
    g_state_id = *reinterpret_cast<int32_t*>(entry);
    SETTINGS_LOG("state entry %d injected (enter/process/f3/f4/event)", g_state_id);
    return true;
}

