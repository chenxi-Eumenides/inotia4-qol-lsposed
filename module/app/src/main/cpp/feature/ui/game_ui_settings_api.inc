
void settings_ui_start_auto_inject() {
    ensure_inject_thread();
}

void settings_register_config_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    if (g_config_bridge_class != nullptr) {
        env->DeleteGlobalRef(g_config_bridge_class);
    }
    g_config_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
}

std::string data_settings_ui_inject() {
    if (!inject_state_entry_locked()) return op_err("state entry inject failed");
    ensure_inject_thread();
    return op_ok();
}

std::string data_settings_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    void* btn = more_games_btn();
    int64_t bx = -1, by = -1, bw = -1, bh = -1;
    if (btn != nullptr) {
        uint8_t* co = reinterpret_cast<uint8_t*>(btn);
        bx = *reinterpret_cast<int64_t*>(co + CO_RECT_X);
        by = *reinterpret_cast<int64_t*>(co + CO_RECT_Y);
        bw = *reinterpret_cast<int64_t*>(co + CO_RECT_W);
        bh = *reinterpret_cast<int64_t*>(co + CO_RECT_H);
    }
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{"
        "\"more_games_injected\":%s,\"more_games_btn\":\"%p\","
        "\"more_games_rect\":[%lld,%lld,%lld,%lld],"
        "\"state_id\":%d,\"state_injected\":%s,"
        "\"panel_active\":%s,\"root\":\"%p\","
        "\"stack_limit_enabled\":%s,"
        "\"screen\":\"%s\"}",
        g_more_games_injected ? "true" : "false", btn,
        static_cast<long long>(bx), static_cast<long long>(by),
        static_cast<long long>(bw), static_cast<long long>(bh),
        g_state_id, g_state_entry != nullptr ? "true" : "false",
        g_panel_active ? "true" : "false", g_root,
        stack_limit_enabled() ? "true" : "false",
        data_ui_screen());
    return std::string(buf);
}

std::string data_settings_ui_restore() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_state_entry != nullptr) {
        memcpy(g_state_entry, g_state_backup, POPUP_STATE_SIZE);
    g_state_entry = nullptr;
        g_state_id = -1;
    }
    g_more_games_hook.uninstall();
    g_more_games_injected = false;
    g_panel_active = false;
    if (g_option_images_loaded) {
        ui_original::unload_option_images();
        g_option_images_loaded = false;
    }
    return op_ok();
}

std::string data_settings_ui_open_panel() {
    if (g_base == 0) return op_err("libgame not ready");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    if (g_state_id < 0) return op_err("state not injected");
    fn_ui_set_popup_process_info(1, g_state_id);
    SETTINGS_LOG("open settings panel state id=%d", g_state_id);
    return op_ok();
}

std::string data_settings_ui_open_option() {
    if (g_base == 0) return op_err("libgame not ready");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    uint8_t* entry = find_state_entry(F_PANEL_OPTIONS_ENTER);
    if (entry == nullptr) return op_err("option state not found");
    int id = *reinterpret_cast<int32_t*>(entry);
    fn_ui_set_popup_process_info(1, id);
    SETTINGS_LOG("open option state id=%d", id);
    return op_ok();
}
