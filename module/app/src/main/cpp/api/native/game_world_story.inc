bool data_story_active() {
    if (g_base == 0) return false;
    uint32_t gs = g_gamestate != nullptr ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0;
    return gs == 1;
}

std::string data_story_json() {
    if (g_base == 0) return "{\"active\":false}";
    std::string out = "{\"active\":true";
    void* teller = *reinterpret_cast<void**>(g_base + G_EVT_PTELLER_VMA);
    if (teller != nullptr && fn_get_name != nullptr) {
        char* name = fn_get_name(teller);
        if (name != nullptr) out += ",\"speaker\":\"" + json_escape(name) + "\"";
    }
    uint8_t* pt = *reinterpret_cast<uint8_t**>(g_base + G_EVT_PTEXT_VMA);
    if (pt != nullptr) {
        std::string text;
        for (int i = 0; i < 2048 && pt[i] != 0; ++i) text += static_cast<char>(pt[i]);
        out += ",\"text\":\"" + json_escape(text.c_str()) + "\"";
    }
    uint32_t idx = *reinterpret_cast<uint32_t*>(g_base + G_EVT_INDEX_VMA);
    uint32_t cnt = *reinterpret_cast<uint32_t*>(g_base + G_EVT_DATA_COUNT_VMA);
    out += ",\"index\":" + std::to_string(idx) + ",\"count\":" + std::to_string(cnt);
    out += "}";
    return out;
}

