#include "host/settings_config_bridge.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace qol::host {
namespace {
using namespace contracts;
constexpr std::array<const char*, kSettingsConfigBoolCount> kKeys{
    "stackLimitIncrease", "moveMergeEnabled", "opEnabled", "extensionBagEnabled",
    "customRecipeEnabled", "autoSellEnabled", "apiEnabled", "debugLogEnabled",
    "simpleModeEnabled", "pushAllEnabled"};

QolResult fail(int32_t code, const char* message) {
    return error(code, message, static_cast<uint32_t>(std::strlen(message)));
}
int key_index(const char* key) {
    if (key != nullptr) {
        for (uint32_t i = 0; i < kKeys.size(); ++i) {
            if (std::strcmp(key, kKeys[i]) == 0) return static_cast<int>(i);
        }
    }
    return -1;
}
bool is_bool(int32_t value) { return value == 0 || value == 1; }
struct Request {
    SettingsConfigRequestStatus status{};
    int key = 0;
    bool dispatched = false;
};
struct Bridge {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<int32_t, kSettingsConfigBoolCount> mirror{};
    std::array<Request, kSettingsConfigRequestCapacity> requests{};
    uint64_t revision = 0;
    uint64_t next_id = 1;
    SettingsConfigDispatch dispatch = nullptr;
    void* context = nullptr;
    bool stopping = false;
    std::thread worker;
    ~Bridge() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        changed.notify_all();
        if (worker.joinable()) worker.join();
    }
};
Bridge& bridge() { static Bridge instance; return instance; }

QolResult read_bool(const char* key, int32_t* out) {
    if (key == nullptr || out == nullptr) return fail(kQolErrInvalidArgument, "null argument");
    const int index = key_index(key);
    if (index < 0) return fail(kQolErrNotFound, "unknown public bool key");
    auto& b = bridge();
    std::lock_guard<std::mutex> lock(b.mutex);
    if (b.revision == 0) return fail(kQolErrNotReady, "Kotlin configuration not published");
    *out = b.mirror[index];
    return ok();
}

QolResult request_set_bool(const char* key, int32_t desired, uint64_t* out) {
    if (out != nullptr) *out = 0;
    if (key == nullptr || out == nullptr || !is_bool(desired)) {
        return fail(kQolErrInvalidArgument, "desired must be 0 or 1");
    }
    const int index = key_index(key);
    if (index < 0) return fail(kQolErrNotFound, "unknown public bool key");
    auto& b = bridge();
    std::lock_guard<std::mutex> lock(b.mutex);
    if (b.dispatch == nullptr || b.revision == 0 || b.stopping) {
        return fail(kQolErrNotReady, "settings configuration bridge not ready");
    }
    Request* slot = nullptr;
    for (auto& r : b.requests) {
        if (r.status.request_id == 0) { slot = &r; break; }
        if (r.status.state != SettingsConfigRequestState::kQueued &&
            (slot == nullptr || r.status.request_id < slot->status.request_id)) slot = &r;
    }
    if (slot == nullptr) return fail(kQolErrBlocked, "settings request capacity exhausted");
    *slot = Request{};
    slot->key = index;
    slot->status.size = sizeof(SettingsConfigRequestStatus);
    slot->status.request_id = b.next_id++;
    slot->status.desired_value = desired;
    slot->status.configured_value = b.mirror[index];
    *out = slot->status.request_id;
    b.changed.notify_one();
    return ok();
}

QolResult query_request(uint64_t id, SettingsConfigRequestStatus* out) {
    if (id == 0 || out == nullptr || out->size < sizeof(SettingsConfigRequestStatus)) {
        return fail(kQolErrInvalidArgument, "invalid request/status size");
    }
    auto& b = bridge();
    std::lock_guard<std::mutex> lock(b.mutex);
    for (const auto& r : b.requests) {
        if (r.status.request_id == id) { *out = r.status; return ok(); }
    }
    return fail(id < b.next_id ? kQolErrCancelled : kQolErrNotFound,
                id < b.next_id ? "request receipt expired" : "unknown request");
}

void dispatch_loop() {
    auto& b = bridge();
    std::unique_lock<std::mutex> lock(b.mutex);
    for (;;) {
        Request* next = nullptr;
        for (auto& r : b.requests) {
            if (r.status.request_id != 0 && !r.dispatched &&
                r.status.state == SettingsConfigRequestState::kQueued &&
                (next == nullptr || r.status.request_id < next->status.request_id)) next = &r;
        }
        if (b.stopping) return;
        if (next == nullptr) { b.changed.wait(lock); continue; }
        const auto status = next->status;
        const auto* key = kKeys[next->key];
        next->dispatched = true;
        const auto dispatch = b.dispatch;
        void* context = b.context;
        lock.unlock();
        const auto result = dispatch(context, status.request_id, key, status.desired_value);
        if (!succeeded(result)) {
            settings_config_complete(status.request_id, 0, status.configured_value,
                                     SettingsConfigApplyState::kNotSubmitted, result.code,
                                     "background bridge dispatch failed", "not submitted");
        }
        lock.lock();
    }
}
}  // namespace

const QolSettingsConfigApi& settings_config_api() {
    static const QolSettingsConfigApi api{sizeof(QolSettingsConfigApi), kSettingsConfigMajor,
        kSettingsConfigMinor, &read_bool, &request_set_bool, &query_request};
    return api;
}
const char* settings_config_key(uint32_t index) { return index < kKeys.size() ? kKeys[index] : nullptr; }

QolResult settings_config_install(SettingsConfigDispatch dispatch, void* context) {
    if (dispatch == nullptr) return fail(kQolErrInvalidArgument, "null dispatch");
    auto& b = bridge();
    std::lock_guard<std::mutex> lock(b.mutex);
    if (b.dispatch != nullptr) {
        return b.dispatch == dispatch && b.context == context ? ok() :
               fail(kQolErrConflict, "bridge already installed");
    }
    b.stopping = false;
    b.dispatch = dispatch;
    b.context = context;
    b.worker = std::thread(&dispatch_loop);
    return ok();
}

QolResult settings_config_publish(const int32_t* values, uint32_t count, uint64_t revision) {
    if (values == nullptr || count != kKeys.size() || revision == 0) {
        return fail(kQolErrInvalidArgument, "invalid bool mirror");
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!is_bool(values[i])) return fail(kQolErrInvalidArgument, "invalid mirror bool");
    }
    auto& b = bridge();
    std::lock_guard<std::mutex> lock(b.mutex);
    if (revision <= b.revision) return fail(kQolErrConflict, "stale configuration revision");
    std::copy_n(values, count, b.mirror.begin());
    b.revision = revision;
    return ok();
}

QolResult settings_config_complete(uint64_t id, int32_t persisted, int32_t configured,
                                   SettingsConfigApplyState apply, int32_t error_code,
                                   const char* error_text, const char* apply_message) {
    if (id == 0 || !is_bool(persisted) || !is_bool(configured) ||
        static_cast<uint32_t>(apply) > static_cast<uint32_t>(SettingsConfigApplyState::kUnchanged) ||
        error_text == nullptr || apply_message == nullptr) return fail(kQolErrInvalidArgument, "invalid receipt");
    auto& b = bridge();
    std::lock_guard<std::mutex> lock(b.mutex);
    for (auto& r : b.requests) {
        if (r.status.request_id != id) continue;
        if (r.status.state != SettingsConfigRequestState::kQueued) {
            return fail(kQolErrConflict, "request already completed");
        }
        auto& s = r.status;
        s.persisted = persisted;
        s.configured_value = configured;
        s.apply_state = apply;
        s.error_code = error_code;
        s.state = persisted != 0 && error_code == 0 && apply != SettingsConfigApplyState::kRejected ?
                  SettingsConfigRequestState::kSucceeded : SettingsConfigRequestState::kFailed;
        std::strncpy(s.error, error_text, sizeof(s.error) - 1);
        std::strncpy(s.apply_message, apply_message, sizeof(s.apply_message) - 1);
        return ok();
    }
    return fail(kQolErrNotFound, "unknown request receipt");
}

void settings_config_shutdown() {
    auto& b = bridge();
    { std::lock_guard<std::mutex> lock(b.mutex); b.stopping = true; }
    b.changed.notify_all();
    if (b.worker.joinable()) b.worker.join();
}

#ifdef __ANDROID__
namespace {
JavaVM* g_vm = nullptr;
jclass g_bridge_class = nullptr;
jmethodID g_enqueue = nullptr;
QolResult dispatch_jni(void*, uint64_t id, const char* key, int32_t desired) {
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return fail(kQolErrNotReady, "attach configuration dispatcher failed");
    }
    jstring java_key = env->NewStringUTF(key);
    jboolean accepted = JNI_FALSE;
    if (java_key != nullptr) {
        accepted = env->CallStaticBooleanMethod(g_bridge_class, g_enqueue,
                    static_cast<jlong>(id), java_key, desired == 1 ? JNI_TRUE : JNI_FALSE);
        env->DeleteLocalRef(java_key);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        accepted = JNI_FALSE;
    }
    g_vm->DetachCurrentThread();
    return accepted == JNI_TRUE ? ok() : fail(kQolErrInternal, "Kotlin writer rejected request");
}
}  // namespace
bool settings_config_register_jni(JNIEnv* env, jclass bridge_class) {
    if (g_bridge_class != nullptr) return env->IsSameObject(g_bridge_class, bridge_class);
    if (bridge_class == nullptr || env->GetJavaVM(&g_vm) != JNI_OK) return false;
    auto enqueue = env->GetStaticMethodID(bridge_class, "enqueueSettingsConfigRequest", "(JLjava/lang/String;Z)Z");
    if (enqueue == nullptr) return false;  // pending lookup exception propagates to Kotlin init
    auto ref = static_cast<jclass>(env->NewGlobalRef(bridge_class));
    if (ref == nullptr) return false;
    g_bridge_class = ref;
    g_enqueue = enqueue;
    const auto result = settings_config_install(&dispatch_jni, nullptr);
    if (!succeeded(result)) {
        env->DeleteGlobalRef(g_bridge_class);
        g_bridge_class = nullptr;
        return false;
    }
    return true;
}
#endif
}  // namespace qol::host
