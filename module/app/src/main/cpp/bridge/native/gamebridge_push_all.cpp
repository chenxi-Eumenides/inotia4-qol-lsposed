// gamebridge_push_all.cpp —— 推怪全局开关 JNI 兼容导出。
// 全局配置写入必须走 core.settings native single-key owner-token 路径；此旧 setter 不再写配置。

#include <jni.h>

#include "contracts/commands.h"
#include "infra/commands.h"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetPushAllEnabled(JNIEnv*, jclass, jboolean enabled) {
    // Frozen compatibility stub.
    (void)enabled;
    return JNI_FALSE;
}
