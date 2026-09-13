// autosell_store.cpp —— 自动出售按存档配置持久化（sidecar section `autosell`，直写）。
//
// 桥调用遵循 game_ui_settings_config.inc 的 JVM 获取范式：GetEnv/AttachCurrentThread
// + 缓存 jclass/jmethodID；IO 在 Kotlin ModuleSaveStore（AtomicFile）侧完成。
// 与扩展背包 section/journal 完全隔离，不注册 participant。

#include "feature/autosell/autosell_store.h"

#include "feature/autosell/autosell_config.h"
#include "game_access.h"

#include <android/log.h>

#include <cstring>
#include <mutex>
#include <string>

namespace {

constexpr char kTag[] = "Inotia4AutoSell";
constexpr int kSlotCount = 3;  // ModuleSaveStore SLOT_COUNT

std::mutex g_store_mtx;
jclass g_bridge_class = nullptr;
jmethodID g_load_method = nullptr;
jmethodID g_save_method = nullptr;
int g_loaded_slot = -1;          // 运行时配置当前对应的存档槽
bool g_last_persist_ok = false;  // 最近一次持久化结果

bool valid_slot(int slot) { return slot >= 0 && slot < kSlotCount; }

// 获取当前线程的 JNIEnv（主线程已附加；否则尝试附加）。
JNIEnv* current_env() {
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    }
    return env;
}

struct BridgeHandles {
    jclass cls;
    jmethodID load;
    jmethodID save;
};

BridgeHandles bridge_handles() {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    return {g_bridge_class, g_load_method, g_save_method};
}

bool bridge_load(int slot, std::string* out) {
    JNIEnv* env = current_env();
    if (env == nullptr) return false;
    const BridgeHandles handles = bridge_handles();
    if (handles.cls == nullptr || handles.load == nullptr) return false;
    jstring result = static_cast<jstring>(
        env->CallStaticObjectMethod(handles.cls, handles.load, static_cast<jint>(slot)));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    bool ok = false;
    if (utf != nullptr) {
        *out = utf;
        ok = true;
        env->ReleaseStringUTFChars(result, utf);
    }
    env->DeleteLocalRef(result);
    return ok;
}

bool bridge_save(int slot, const std::string& json) {
    JNIEnv* env = current_env();
    if (env == nullptr) return false;
    const BridgeHandles handles = bridge_handles();
    if (handles.cls == nullptr || handles.save == nullptr) return false;
    jstring payload = env->NewStringUTF(json.c_str());
    if (payload == nullptr) return false;
    jstring result = static_cast<jstring>(env->CallStaticObjectMethod(
        handles.cls, handles.save, static_cast<jint>(slot), payload));
    env->DeleteLocalRef(payload);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    const bool ok = utf != nullptr && std::strcmp(utf, "ok") == 0;
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return ok;
}

}  // namespace

void autosell_store_register_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    jmethodID load_mid =
        env->GetStaticMethodID(bridge_class, "loadConfigJson", "(I)Ljava/lang/String;");
    jmethodID save_mid = env->GetStaticMethodID(
        bridge_class, "saveConfigJson", "(ILjava/lang/String;)Ljava/lang/String;");
    if (load_mid == nullptr || save_mid == nullptr) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, kTag, "autosell store bridge method missing");
        return;
    }
    std::lock_guard<std::mutex> lock(g_store_mtx);
    if (g_bridge_class != nullptr) env->DeleteGlobalRef(g_bridge_class);
    g_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
    g_load_method = load_mid;
    g_save_method = save_mid;
    __android_log_print(ANDROID_LOG_INFO, kTag, "autosell store bridge registered");
}

void autosell_store_ensure_loaded(int slot) {
    if (!valid_slot(slot)) return;

    int loaded = -1;
    bool persisted = false;
    bool needs_load = false;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        loaded = g_loaded_slot;
        persisted = g_last_persist_ok;
        needs_load = (g_loaded_slot != slot);
    }
    if (!needs_load) {
        autosell_set_store_status(slot, loaded, persisted);
        return;
    }

    std::string json;
    if (!bridge_load(slot, &json)) {
        // 桥未注册：不覆盖运行时配置，下帧重试。
        autosell_set_store_status(slot, loaded, persisted);
        return;
    }
    if (json.rfind("error:", 0) == 0) {
        // 存储尚未初始化/读取异常：不覆盖运行时配置，也不标记 loaded，下帧重试。
        autosell_set_store_status(slot, loaded, persisted);
        return;
    }
    autosell::Config cfg;
    const bool parsed = autosell_config_from_json(json.c_str(), &cfg);
    autosell_set_runtime_config(parsed ? cfg : autosell::Config{});
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        g_loaded_slot = slot;
        loaded = slot;
        persisted = g_last_persist_ok;
    }
    autosell_set_store_status(slot, loaded, persisted);
    __android_log_print(ANDROID_LOG_INFO, kTag, "sidecar config load slot=%d parsed=%d",
                        slot, parsed ? 1 : 0);
}

bool autosell_store_persist(int slot, const autosell::Config& config) {
    if (!valid_slot(slot)) return false;
    const std::string json = autosell_config_to_json(config);
    const bool ok = bridge_save(slot, json);
    int loaded = -1;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        g_last_persist_ok = ok;
        loaded = g_loaded_slot;
    }
    autosell_set_store_status(slot, loaded, ok);
    __android_log_print(ANDROID_LOG_INFO, kTag, "sidecar config persist slot=%d ok=%d",
                        slot, ok ? 1 : 0);
    return ok;
}
