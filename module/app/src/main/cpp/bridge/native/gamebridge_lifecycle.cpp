// gamebridge_lifecycle.cpp —— 功能生命周期控制 JNI 薄层（P3）。
//
// 只做参数转换：jstring 插件 id 拷入固定缓冲，构造 kCommandFeatureLifecycle 命令经
// 命令端口投递，由宿主生命周期执行器在主线程下一逻辑帧执行（ARCHITECTURE.md §3.2/§9.1）。
// 返回命令票据（0 = 投递失败）；状态查询走 /api/system/features。
//
// id 拷贝到本地缓冲的原因：commands_port().post 只按值存 QolCommand，命令要到主线程
// drain 才执行，键须由命令槽位自有（infra/commands.cpp 入队时按值拷贝键），此处缓冲
// 在 post 返回后即可释放。

#include <jni.h>

#include <cstdio>

#include "contracts/commands.h"
#include "infra/commands.h"

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativeFeatureLifecycle(JNIEnv* env, jclass, jstring plugin_id,
                                                         jint action) {
    if (plugin_id == nullptr) {
        return 0;
    }
    const char* chars = env->GetStringUTFChars(plugin_id, nullptr);
    if (chars == nullptr) {
        return 0;
    }
    char plugin_id_buf[64] = {};
    // 超长截断：配置/插件查找按截断串失败而非越界。
    std::snprintf(plugin_id_buf, sizeof(plugin_id_buf), "%s", chars);
    env->ReleaseStringUTFChars(plugin_id, chars);

    const qol::contracts::QolCommand command{
        qol::contracts::kCommandFeatureLifecycle,
        plugin_id_buf,
        action,
    };
    qol::contracts::QolCommandTicket ticket = 0;
    const QolResult result = qol::infra::commands_port().post(&ticket, &command);
    return result.ok == 0 ? static_cast<jlong>(ticket) : 0;
}
