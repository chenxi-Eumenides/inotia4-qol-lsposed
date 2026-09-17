// gamebridge_push_all.cpp —— 推怪全局开关 JNI 薄层。
// 只做参数转换：把开关下发为一条命令（kCommandSetConfigBool），由主线程在下一逻辑帧写入配置；
// 插件订阅 kEventConfigChanged 后切门禁，门禁变化由宿主按机制安装/还原资源。
// 返回值表示「本次下发是否成功入队」，不是开关状态（与 nativeSetSimpleModeEnabled 同构）。

#include <jni.h>

#include "contracts/commands.h"
#include "infra/commands.h"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetPushAllEnabled(JNIEnv*, jclass, jboolean enabled) {
    const qol::contracts::QolCommand command{
        qol::contracts::kCommandSetConfigBool,
        "plugin.push_all.enabled",
        enabled == JNI_TRUE ? 1 : 0,
    };
    qol::contracts::QolCommandTicket ticket = 0;
    const QolResult result = qol::infra::commands_port().post(&ticket, &command);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}
