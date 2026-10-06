// 持久配置桥 JNI 薄层（settings_config v1.0）：仅参数/结果转换与字符串搬运，
// 排队、镜像与回执语义全部收口 host/settings_config_bridge.cpp。
#include "host/settings_config_bridge.h"

#include <jni.h>

namespace {
constexpr jint kMirrorCount = static_cast<jint>(qol::contracts::kSettingsConfigBoolCount);

bool load_mirror(JNIEnv* env, jintArray values, jint (&mirror)[qol::contracts::kSettingsConfigBoolCount]) {
    if (env->GetArrayLength(values) != kMirrorCount) return false;
    env->GetIntArrayRegion(values, 0, kMirrorCount, mirror);
    return true;
}
}  // namespace

// Kotlin 持久配置桥初始化：注册 enqueue 反调类 + 首次镜像发布（由 ModuleConfigUiBridge 调用）。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsConfigInit(JNIEnv* env, jclass, jclass bridge_class,
                                                           jintArray values, jlong revision) {
    if (bridge_class == nullptr || values == nullptr) return JNI_FALSE;
    if (!qol::host::settings_config_register_jni(env, bridge_class)) return JNI_FALSE;
    jint mirror[qol::contracts::kSettingsConfigBoolCount];
    if (!load_mirror(env, values, mirror)) return JNI_FALSE;
    const auto result = qol::host::settings_config_publish(mirror, kMirrorCount, static_cast<uint64_t>(revision));
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}

// 配置变更落盘后重发镜像；revision 必须严格递增（native 侧拒绝 stale/回退）。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsConfigPublish(JNIEnv* env, jclass, jintArray values,
                                                              jlong revision) {
    if (values == nullptr) return JNI_FALSE;
    jint mirror[qol::contracts::kSettingsConfigBoolCount];
    if (!load_mirror(env, values, mirror)) return JNI_FALSE;
    const auto result = qol::host::settings_config_publish(mirror, kMirrorCount, static_cast<uint64_t>(revision));
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}

// 后台 writer 完成一次 set 后的回执回传（persisted/configured/apply 语义见 contracts/settings_config.h）。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsConfigComplete(JNIEnv* env, jclass, jlong id, jint persisted,
                                                               jint configured, jint apply_state,
                                                               jint error_code, jstring error,
                                                               jstring apply_message) {
    if (apply_state < 0 ||
        apply_state > static_cast<jint>(qol::contracts::SettingsConfigApplyState::kUnchanged)) {
        return JNI_FALSE;
    }
    const char* error_text = error == nullptr ? nullptr : env->GetStringUTFChars(error, nullptr);
    const char* apply_text =
        apply_message == nullptr ? nullptr : env->GetStringUTFChars(apply_message, nullptr);
    if (error == nullptr || apply_message == nullptr) {
        if (error_text != nullptr) env->ReleaseStringUTFChars(error, error_text);
        if (apply_text != nullptr) env->ReleaseStringUTFChars(apply_message, apply_text);
        return JNI_FALSE;
    }
    const auto result = qol::host::settings_config_complete(
        static_cast<uint64_t>(id), persisted, configured,
        static_cast<qol::contracts::SettingsConfigApplyState>(apply_state), error_code, error_text,
        apply_text);
    env->ReleaseStringUTFChars(error, error_text);
    env->ReleaseStringUTFChars(apply_message, apply_text);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}
