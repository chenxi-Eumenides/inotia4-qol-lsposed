// gamebridge_push_all.cpp —— 推怪全局开关 JNI 薄层。
// 只做参数转换：patch 安装/还原逻辑在 feature/push_all。
// 返回值表示「本次下发是否成功」，不是开关状态（与 nativeSetSimpleModeEnabled 同构）。

#include "feature/push_all/push_all.h"

#include <jni.h>

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetPushAllEnabled(JNIEnv*, jclass, jboolean enabled) {
    return push_all_set_enabled(enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}
