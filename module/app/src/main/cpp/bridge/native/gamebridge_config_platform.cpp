#include "host/features/platform.h"
#include "host/core_settings_config_service.h"

#include <jni.h>

#include <mutex>

namespace {

JavaVM* g_vm = nullptr;
jclass g_callbacks = nullptr;
jmethodID g_update_method = nullptr;
std::mutex g_callback_mutex;

void deliver_platform_update(void*, const qol::host::FeaturePlatformState& state) {
    JavaVM* vm = nullptr;
    jclass callbacks = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_callback_mutex);
        vm = g_vm;
        callbacks = g_callbacks;
        method = g_update_method;
    }
    if (vm == nullptr || callbacks == nullptr || method == nullptr) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    jstring address = env->NewStringUTF(state.listen_address.c_str());
    if (address != nullptr) {
        env->CallStaticVoidMethod(callbacks, method, static_cast<jlong>(state.revision), address,
                                  static_cast<jint>(state.listen_port),
                                  state.api_enabled ? JNI_TRUE : JNI_FALSE,
                                  state.simple_mode ? JNI_TRUE : JNI_FALSE);
        env->DeleteLocalRef(address);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) vm->DetachCurrentThread();
}

bool register_callbacks(JNIEnv* env, jclass callbacks) {
    if (env == nullptr || callbacks == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    if (g_callbacks != nullptr) return env->IsSameObject(g_callbacks, callbacks) == JNI_TRUE;
    if (env->GetJavaVM(&g_vm) != JNI_OK) return false;
    const jmethodID method = env->GetStaticMethodID(
        callbacks, "onConfigPlatformUpdate", "(JLjava/lang/String;IZZ)V");
    if (method == nullptr) return false;
    jclass global = static_cast<jclass>(env->NewGlobalRef(callbacks));
    if (global == nullptr) return false;
    g_callbacks = global;
    g_update_method = method;
    return true;
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeConfigPlatformSubscribe(JNIEnv* env, jclass,
                                                                jclass callbacks) {
    if (!register_callbacks(env, callbacks)) return JNI_FALSE;
    const auto result = qol::host::feature_platform_subscribe(&deliver_platform_update, nullptr);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}

// Kotlin reports a listener bind failure with the revision it observed. The native config
// owner rewrites listen_port to the default (8088) and emits a revision effect, which the
// platform projection delivers back so the server restarts on the fallback port. The export
// signature stays frozen; the second failure on 8088 is rejected to avoid a fallback loop.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeConfigPlatformReportBindFailure(JNIEnv*, jclass,
                                                                         jlong revision, jint failed_port) {
    return qol::host::core_settings_config_service().report_bind_failure(
               static_cast<uint64_t>(revision), static_cast<int>(failed_port))
               ? JNI_TRUE
               : JNI_FALSE;
}
