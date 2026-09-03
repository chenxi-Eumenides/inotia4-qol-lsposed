bool jni_get_config_json(char* out, size_t out_size) {
    if (g_base == 0) return false;
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return false;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    }
    jclass cls = g_config_bridge_class;
    if (cls == nullptr) cls = env->FindClass("com/inotia4/qol/ModuleConfigUiBridge");
    if (cls == nullptr) { env->ExceptionClear(); return false; }
    jmethodID mid = env->GetStaticMethodID(cls, "getConfigJson", "()Ljava/lang/String;");
    if (mid == nullptr) {
        env->ExceptionClear();
        if (g_config_bridge_class == nullptr) env->DeleteLocalRef(cls);
        return false;
    }
    jstring s = static_cast<jstring>(env->CallStaticObjectMethod(cls, mid));
    if (g_config_bridge_class == nullptr) env->DeleteLocalRef(cls);
    if (s == nullptr) { env->ExceptionClear(); return false; }
    const char* utf = env->GetStringUTFChars(s, nullptr);
    if (utf == nullptr) { env->DeleteLocalRef(s); return false; }
    snprintf(out, out_size, "%s", utf);
    env->ReleaseStringUTFChars(s, utf);
    env->DeleteLocalRef(s);
    return true;
}

bool jni_toggle_config(const char* key) {
    if (g_base == 0) return false;
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return false;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    }
    jclass cls = g_config_bridge_class;
    if (cls == nullptr) cls = env->FindClass("com/inotia4/qol/ModuleConfigUiBridge");
    if (cls == nullptr) { env->ExceptionClear(); return false; }
    jmethodID mid = env->GetStaticMethodID(cls, "toggleConfig", "(Ljava/lang/String;)Ljava/lang/String;");
    if (mid == nullptr) {
        env->ExceptionClear();
        if (g_config_bridge_class == nullptr) env->DeleteLocalRef(cls);
        return false;
    }
    jstring jk = env->NewStringUTF(key);
    jstring s = static_cast<jstring>(env->CallStaticObjectMethod(cls, mid, jk));
    env->DeleteLocalRef(jk);
    if (g_config_bridge_class == nullptr) env->DeleteLocalRef(cls);
    bool ok = false;
    if (s != nullptr) {
        const char* utf = env->GetStringUTFChars(s, nullptr);
        if (utf != nullptr) {
            ok = (strcmp(utf, "ok") == 0);
            SETTINGS_LOG("toggle %s -> %s", key, utf);
            env->ReleaseStringUTFChars(s, utf);
        }
        env->DeleteLocalRef(s);
    }
    env->ExceptionClear();
    return ok;
}

void parse_config_into_rows(const char* json) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        const char* p = strstr(json, kRowKeys[i]);
        if (p == nullptr) continue;
        p = strchr(p, ':');
        if (p == nullptr) continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        snprintf(g_row_status[i], CB_TEXT_SIZE, "%s", (*p == 't' || *p == '1') ? "开" : "关");
    }
    const char* addr = strstr(json, "listenAddress");
    if (addr != nullptr) {
        const char* colon = strchr(addr, ':');
        if (colon != nullptr) {
            const char* q1 = strchr(colon, '"');
            if (q1 != nullptr) {
                const char* q2 = strchr(q1 + 1, '"');
                if (q2 != nullptr) {
                    size_t len = static_cast<size_t>(q2 - q1 - 1);
                    if (len >= CB_TEXT_SIZE) len = CB_TEXT_SIZE - 1;
                    memcpy(g_addr_text, q1 + 1, len);
                    g_addr_text[len] = 0;
                }
            }
        }
    }
    const char* port = strstr(json, "listenPort");
    if (port != nullptr) {
        const char* colon = strchr(port, ':');
        if (colon != nullptr) {
            char* end = nullptr;
            long v = strtol(colon + 1, &end, 10);
            if (end != nullptr && end != colon + 1) {
                snprintf(g_addr_text + strlen(g_addr_text), CB_TEXT_SIZE - strlen(g_addr_text),
                         ":%ld", v);
            }
        }
    }
}

// ---- 面板绘制（DrawProc：色块 + 文字）----

