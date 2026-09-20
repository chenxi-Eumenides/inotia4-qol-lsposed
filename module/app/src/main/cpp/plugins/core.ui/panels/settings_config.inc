bool jni_toggle_config(const char* key) {
    if (g_sdk == nullptr || g_sdk->config.get_bool == nullptr ||
        g_sdk->config.set_bool == nullptr || key == nullptr) return false;
    int32_t current = 0;
    if (!contracts::succeeded(g_sdk->config.get_bool(key, 0, &current))) return false;
    const QolResult result = g_sdk->config.set_bool(key, current == 0 ? 1 : 0);
    if (contracts::succeeded(result)) SETTINGS_LOG("toggle %s -> %s", key, current == 0 ? "1" : "0");
    return contracts::succeeded(result);
}

bool sync_config_into_rows() {
    if (g_sdk == nullptr || g_sdk->config.get_bool == nullptr) return false;
    for (int i = 0; i < kSettingsItemCount; ++i) {
        const char* key = kSettingsItems[i].key;
        if (key == nullptr || key[0] == 0) continue;
        int32_t value = 0;
        if (!contracts::succeeded(g_sdk->config.get_bool(key, 0, &value))) return false;
        snprintf(g_row_status[i], CB_TEXT_SIZE, "%s", value != 0 ? "开" : "关");
    }
    snprintf(g_header_text, CB_TEXT_SIZE, "QoL模块 v0.1.0");
    return true;
}

// ---- 面板绘制（DrawProc：色块 + 文字）----
