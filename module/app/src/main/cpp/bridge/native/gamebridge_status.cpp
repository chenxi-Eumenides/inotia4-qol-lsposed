// gamebridge_status.cpp —— P1 最小诊断接口 JNI 薄层。
//
// 本文件是 P1 起按 host/ 分组的新 JNI 薄层；既有导出仍留在 bridge/native/ 下，
// 后续收口迁移。仅做返回值转换，JSON 构造在 host/status_json.cpp。
// Kotlin 侧方法名已冻结：nativeFeatureStatusJson。

#include <jni.h>

#include <string>

#include "host/status_json.h"
#include "host/http_registry.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeFeatureStatusJson(JNIEnv* env, jclass) {
    const std::string json = qol::host::host_feature_status_json();
    jstring result = env->NewStringUTF(json.c_str());
    if (result != nullptr) {
        return result;
    }
    // NewStringUTF 失败（OOM）时不得抛异常、不得返回 null，退回错误 JSON。
    return env->NewStringUTF("{\"ok\":false,\"error\":\"oom\"}");
}

// P3 资源登记表导出（/api/system/resources）：与 features 同模式。
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSystemResourcesJson(JNIEnv* env, jclass) {
    const std::string json = qol::host::host_resources_json();
    jstring result = env->NewStringUTF(json.c_str());
    if (result != nullptr) {
        return result;
    }
    return env->NewStringUTF("{\"ok\":false,\"error\":\"oom\"}");
}

// P3 命令票据状态（/api/system/tickets/{id}）：ticket 为 jlong，0 由 host 判为无效。
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeTicketStateJson(JNIEnv* env, jclass, jlong ticket) {
    const std::string json = qol::host::host_ticket_json(static_cast<uint64_t>(ticket));
    jstring result = env->NewStringUTF(json.c_str());
    if (result != nullptr) {
        return result;
    }
    return env->NewStringUTF("{\"ok\":false,\"error\":\"oom\"}");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeHttpRoutesJson(JNIEnv* env, jclass) {
    const std::string json = qol::host::host_http_routes_json();
    jstring result = env->NewStringUTF(json.c_str());
    if (result != nullptr) {
        return result;
    }
    return env->NewStringUTF("{\"ok\":false,\"error\":\"oom\"}");
}
