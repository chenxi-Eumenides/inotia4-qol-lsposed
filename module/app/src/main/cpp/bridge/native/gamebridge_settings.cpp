#include "gamebridge_internal.h"

// ---- 模块设置 UI 端点（ui-settings v0.6.9）----
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiInject(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_inject().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_status_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiRestore(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_restore().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiOpenOption(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_open_option().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsUiOpenPanel(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_open_panel().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExtensionBagUiStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_virtual_bag_ui_status_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExtensionBagTestEquip(JNIEnv* env, jclass, jint index, jint capacity) {
    return env->NewStringUTF(data_virtual_bag_test_equip(static_cast<int>(index), static_cast<int>(capacity)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExtensionBagTestItem(JNIEnv* env, jclass, jint index, jint slot,
                                                                jint category, jint count) {
    return env->NewStringUTF(data_virtual_bag_test_item(static_cast<int>(index), static_cast<int>(slot),
                                                        static_cast<int>(category), static_cast<int>(count)).c_str());
}

