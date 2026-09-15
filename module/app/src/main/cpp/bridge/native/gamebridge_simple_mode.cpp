// gamebridge_simple_mode.cpp —— 简单模式 JNI 薄层。
// 只做参数转换：布尔开关写入 native 侧原子量（业务逻辑在 feature/simple_mode）。
// 为什么需要这条独立通道：伤害 hook 是热路径，不能每次去读 config.json 或反调 Kotlin，
// 必须由一个 C++ 原子量承载（与 nativeSetStackLimitEnabled 等既有开关同构）。

#include "feature/simple_mode/simple_mode.h"

#include <jni.h>

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetSimpleModeEnabled(JNIEnv*, jclass, jboolean enabled) {
    // 与 nativeSetStackLimitEnabled 等同构：返回值表示「本次下发是否成功」，不是开关状态。
    // 早前实现返回 simple_mode_enabled()，导致关闭时日志出现 "applied=false"（读起来像失败）。
    simple_mode_set_enabled(enabled == JNI_TRUE);
    return JNI_TRUE;
}
