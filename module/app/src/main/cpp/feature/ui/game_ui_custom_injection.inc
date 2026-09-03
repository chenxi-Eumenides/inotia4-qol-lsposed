// ---- state list 条目定位与注入 ----

uint8_t* find_state_entry(uintptr_t enter_vma) {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || *got == nullptr) return nullptr;
    uint8_t* list = reinterpret_cast<uint8_t*>(*got);
    for (int i = 0; i < 27; ++i) {
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * POPUP_STATE_SIZE + 0x10);
        if (enter == g_base + enter_vma) return list + i * POPUP_STATE_SIZE;
    }
    return nullptr;
}

bool inject_state_entry() {
    std::lock_guard<std::mutex> lock(g_custom_mtx);
    if (g_state_entry != nullptr) return true;
    if (g_base == 0) return false;
    uint8_t* entry = find_state_entry(F_PANEL_UNK1_ENTER);  // INAP_GEMSHOP（IAP 屏蔽=死条目）
    if (entry == nullptr) {
        CUSTOM_LOG("state entry (INAP_GEMSHOP) not found");
        return false;
    }
    memcpy(g_state_backup, entry, POPUP_STATE_SIZE);
    *reinterpret_cast<uintptr_t*>(entry + 0x10) = reinterpret_cast<uintptr_t>(&custom_panel_enter);
    *reinterpret_cast<uintptr_t*>(entry + 0x18) = reinterpret_cast<uintptr_t>(&custom_panel_process);
    *reinterpret_cast<uintptr_t*>(entry + 0x28) = reinterpret_cast<uintptr_t>(&custom_panel_f3);
    *reinterpret_cast<uintptr_t*>(entry + 0x30) = reinterpret_cast<uintptr_t>(&custom_panel_f4);
    *reinterpret_cast<uintptr_t*>(entry + 0x38) = reinterpret_cast<uintptr_t>(&custom_panel_event);
    g_state_entry = entry;
    g_state_id = *reinterpret_cast<int32_t*>(entry);
    CUSTOM_LOG("state entry %d injected (enter/process/f3/f4/event)", g_state_id);
    return true;
}

// ---- 商店返回按钮替换 ----

void* store_back_btn() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<void**>(g_base + G_UISTORE_VMA + UISTORE_SLOT_BACK_BTN);
}

void custom_store_btn_clicked(void* ctrl) {
    CUSTOM_LOG("store back btn clicked -> open custom panel");
    if (fn_ui_set_popup_process_info == nullptr || g_state_id < 0) return;
    fn_ui_set_popup_process_info(1, g_state_id);
}

bool inject_store_back_btn() {
    std::lock_guard<std::mutex> lock(g_custom_mtx);
    if (g_btn_injected) return true;
    if (g_base == 0) return false;
    void* btn = store_back_btn();
    if (btn == nullptr) return false;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(btn) + CO_DATA);
    if (data == nullptr) return false;
    void** slot = reinterpret_cast<void**>(data + CB_EXECUTE_PROC);
    if (*slot == reinterpret_cast<void*>(&custom_store_btn_clicked)) {
        g_btn_injected = true;
        return true;
    }
    if (*slot == nullptr) return false;
    if (!g_store_back_hook.install_typed(slot, &custom_store_btn_clicked)) {
        return false;
    }
    g_btn_injected = true;
    CUSTOM_LOG("store back btn ExecuteProc hooked, ctrl=%p orig=%p", btn, g_store_back_hook.orig);
    return true;
}

void ensure_inject_thread() {
    if (g_thread_started.exchange(true)) return;
    std::thread([]() {
        for (;;) {
            if (g_btn_want.load()) {
                bool store_open = data_popup_top_vma() == F_PANEL_SHOP_ENTER;  // 相对偏移
                if (store_open) {
                    inject_state_entry();
                    inject_store_back_btn();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}

}  // namespace

std::string data_custom_btn_inject() {
    g_btn_want.store(true);
    ensure_inject_thread();
    if (g_state_id >= 0) {
        if (inject_store_back_btn()) return op_ok();
        return op_err("store panel not open");
    }
    return op_err("state entry not injected (wait for store panel)");
}

std::string data_custom_panel_open() {
    if (g_state_id < 0 && !inject_state_entry()) return op_err("state entry inject failed");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    fn_ui_set_popup_process_info(1, g_state_id);
    return op_ok();
}

std::string data_custom_restore() {
    std::lock_guard<std::mutex> lock(g_custom_mtx);
    g_btn_want.store(false);
    if (g_btn_injected) {
        // 商店关闭后旧按钮已释放：uninstall 会写已释放内存 → 仅当商店仍打开时还原
        if (data_popup_top_vma() == F_PANEL_SHOP_ENTER) {  // 相对偏移
            g_store_back_hook.uninstall();
        }
        g_btn_injected = false;
    }
    if (g_state_entry != nullptr) {
        memcpy(g_state_entry, g_state_backup, POPUP_STATE_SIZE);
        g_state_entry = nullptr;
        g_state_id = -1;
    }
    g_panel_active = false;  // 控件树保留复用（DeleteControl SIGBUS，见 custom_panel_f3）
    return op_ok();
}

std::string data_custom_status_json() {
    std::lock_guard<std::mutex> lock(g_custom_mtx);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{"
        "\"btn_injected\":%s,\"store_btn\":\"%p\","
        "\"state_id\":%d,\"state_injected\":%s,"
        "\"panel_active\":%s,\"root\":\"%p\","
        "\"stack_limit_enabled\":%s,"
        "\"screen\":\"%s\"}",
        g_btn_injected ? "true" : "false", store_back_btn(),
        g_state_id, g_state_entry != nullptr ? "true" : "false",
        g_panel_active ? "true" : "false", g_root,
        stack_limit_enabled() ? "true" : "false",
        data_ui_screen());
    return std::string(buf);
}
