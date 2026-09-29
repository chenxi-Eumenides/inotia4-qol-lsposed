#include "gamebridge_internal.h"

// 面板插件 API（D1-b2 迁移：core.ui 设置页容器 / save_backup 备份面板）。
#if !QOL_KERNEL_ONLY
using qol::features::core_ui::data_settings_ui_inject;
using qol::features::core_ui::data_settings_ui_open_option;
using qol::features::core_ui::data_settings_ui_open_panel;
using qol::features::core_ui::data_settings_ui_restore;
using qol::features::core_ui::data_settings_ui_status_json;
using qol::features::save_backup::data_savebackup_ui_inject;
using qol::features::save_backup::data_savebackup_ui_open_panel;
using qol::features::save_backup::data_savebackup_ui_restore;
using qol::features::save_backup::data_savebackup_ui_self_check;
using qol::features::save_backup::data_savebackup_ui_status_json;
#endif

// ---- 模块设置 UI 端点（ui-settings v0.6.9）----
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiInject(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_settings_ui_inject().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiStatus(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_settings_ui_status_json().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiRestore(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_settings_ui_restore().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiOpenOption(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_settings_ui_open_option().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiOpenPanel(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_settings_ui_open_panel().c_str());
#endif
}

// ---- 存档管理器游戏内面板（ui-savebackup v0.7.x）----
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveBackupUiInject(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_savebackup_ui_inject().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveBackupUiStatus(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_savebackup_ui_status_json().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveBackupUiRestore(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_savebackup_ui_restore().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveBackupUiOpenPanel(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_savebackup_ui_open_panel().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveBackupUiSelfCheck(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_savebackup_ui_self_check().c_str());
#endif
}
