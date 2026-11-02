// gamebridge_variant.cpp —— P1 游戏变体诊断接口 JNI 薄层。
//
// 本文件是 P1 起按 host/ 分组的新 JNI 薄层；既有导出仍留在 bridge/native/ 下，
// 后续收口迁移。仅做返回值转换，JSON 构造在 host/variant_json.cpp。
// Kotlin 侧方法名已冻结：nativeGetVariantJson。

#include <jni.h>

#include <string>

#include "host/variant_json.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetVariantJson(JNIEnv* env, jclass) {
    const std::string json = qol::host::host_variant_json();
    jstring result = env->NewStringUTF(json.c_str());
    if (result != nullptr) {
        return result;
    }
    // NewStringUTF 失败（OOM）时不得抛异常、不得返回 null，退回未知变体 JSON。
    return env->NewStringUTF(
        "{\"series\":\"unknown\",\"version\":\"\",\"known\":false,\"capabilities\":0}");
}
