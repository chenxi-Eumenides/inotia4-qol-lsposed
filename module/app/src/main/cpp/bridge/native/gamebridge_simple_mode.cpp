// gamebridge_simple_mode.cpp —— 简单模式 JNI 薄层。
// 只做参数转换：把开关下发为一条命令（kCommandSetConfigBool），由主线程在下一逻辑帧
// 写入配置；插件订阅 kEventConfigChanged 后据此切换门禁。
//
// 为什么改走命令端口：游戏状态写入口统一经主线程命令队列（ARCHITECTURE.md §6.1），
// 取代原先 JVM 线程直写 C++ 原子量；代价是 ≤1 帧延迟（命令在主线程 drain 时执行）。
// 命令端口的返回值仍表示「本次下发是否成功入队」，不是开关状态（与
// nativeSetStackLimitEnabled 等同构）。

#include <jni.h>

#include "contracts/commands.h"
#include "infra/commands.h"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetSimpleModeEnabled(JNIEnv*, jclass, jboolean enabled) {
    const qol::contracts::QolCommand command{
        qol::contracts::kCommandSetConfigBool,
        "plugin.simple_mode.enabled",
        enabled == JNI_TRUE ? 1 : 0,
    };
    qol::contracts::QolCommandTicket ticket = 0;
    const QolResult result = qol::infra::commands_port().post(&ticket, &command);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}
