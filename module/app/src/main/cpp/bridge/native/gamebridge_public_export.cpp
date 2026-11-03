// 公共 Download 导出的 JNI 出口：Kotlin PublicExportBridge 注册静态方法后，
// native 侧经这些静态方法把导出任务交给 Kotlin 单线程 executor（复制与用户反馈都在 Kotlin 侧）。
#include "host/public_export.h"

#include <jni.h>

#include <mutex>
#include <string>

#include "infra/log/qol_log.h"

// gamebridge.cpp 定义（JNI_OnLoad 缓存），kernel-only 构建同样存在。
JavaVM* g_jvm();

namespace qol::host {
namespace {

std::mutex g_mutex;
jclass g_bridge_class = nullptr;
jmethodID g_export_crash_logs = nullptr;
jmethodID g_export_backup = nullptr;
jmethodID g_pick_import_file = nullptr;

// 取当前线程 JNIEnv；线程未挂到 JVM 时临时 attach，attached=true 时调用方负责 detach。
JNIEnv* current_env(bool* attached) {
    *attached = false;
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
#if defined(__ANDROID__)
    if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
#else
    if (jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) return nullptr;
#endif
    *attached = true;
    return env;
}

bool bridge_registered_locked() {
    return g_bridge_class != nullptr && g_export_crash_logs != nullptr &&
           g_export_backup != nullptr && g_pick_import_file != nullptr;
}

// 统一调用转体：Kotlin 侧方法只向 executor 入队后立即返回，因此在锁内调用不会
// 长时间持锁；返回 false 表示未注册、env 不可用或 Java 侧抛出异常。
template <typename Call>
bool call_bridge(const char* what, Call&& call) {
    bool attached = false;
    JNIEnv* env = current_env(&attached);
    if (env == nullptr) {
        QOL_LOG_ERROR(QolDomain::kPlatform, "public export %s skipped: jni env unavailable", what);
        return false;
    }
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (bridge_registered_locked()) {
            call(env);
            ok = !env->ExceptionCheck();
            if (!ok) {
                // 必须清掉 pending exception，否则下一个 JNI 入口立即爆异常。
                env->ExceptionClear();
                QOL_LOG_ERROR(QolDomain::kPlatform, "public export %s failed: java exception", what);
            }
        }
    }
    if (attached) g_jvm()->DetachCurrentThread();
    return ok;
}

#if !QOL_KERNEL_ONLY
// 缓存 Kotlin bridge 的全局引用与静态方法 ID；重复注册时先释放旧全局引用。
// 方法解析全部成功才替换注册，任一缺失时保持旧注册不变。
void register_bridge(JNIEnv* env, jclass bridge) {
    jmethodID crash_logs = env->GetStaticMethodID(bridge, "exportCrashLogs", "()V");
    jmethodID backup =
        env->GetStaticMethodID(bridge, "exportBackup", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID pick = env->GetStaticMethodID(bridge, "pickImportFile", "()V");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        QOL_LOG_ERROR(QolDomain::kPlatform, "public export bridge register failed: method lookup");
        return;
    }
    if (crash_logs == nullptr || backup == nullptr || pick == nullptr) {
        QOL_LOG_ERROR(QolDomain::kPlatform, "public export bridge register failed: method missing");
        return;
    }
    jobject global = env->NewGlobalRef(bridge);
    if (global == nullptr) {
        QOL_LOG_ERROR(QolDomain::kPlatform, "public export bridge register failed: global ref");
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_bridge_class != nullptr) env->DeleteGlobalRef(g_bridge_class);
    g_bridge_class = static_cast<jclass>(global);
    g_export_crash_logs = crash_logs;
    g_export_backup = backup;
    g_pick_import_file = pick;
}
#endif

}  // namespace

bool public_export_call_crash_logs() {
    return call_bridge("exportCrashLogs", [](JNIEnv* env) {
        env->CallStaticVoidMethod(g_bridge_class, g_export_crash_logs);
    });
}

bool public_export_call_backup(const char* source_path, const char* display_name) {
    if (source_path == nullptr || display_name == nullptr) return false;
    return call_bridge("exportBackup", [&](JNIEnv* env) {
        jstring path = env->NewStringUTF(source_path);
        jstring name = env->NewStringUTF(display_name);
        if (path != nullptr && name != nullptr) {
            env->CallStaticVoidMethod(g_bridge_class, g_export_backup, path, name);
        }
        if (path != nullptr) env->DeleteLocalRef(path);
        if (name != nullptr) env->DeleteLocalRef(name);
    });
}

bool public_export_call_pick_file() {
    return call_bridge("pickImportFile", [](JNIEnv* env) {
        env->CallStaticVoidMethod(g_bridge_class, g_pick_import_file);
    });
}

}  // namespace qol::host

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRegisterPublicExportBridge(JNIEnv* env, jclass, jclass bridge) {
#if !QOL_KERNEL_ONLY
    if (env == nullptr || bridge == nullptr) return;
    qol::host::register_bridge(env, bridge);
#else
    // kernel-only 构建不装配导出链路：不注册，public_export_call_* 一律返回 false（kQolErrNotReady）。
    (void)env;
    (void)bridge;
#endif
}
