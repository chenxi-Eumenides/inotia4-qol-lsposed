#include "gamebridge_internal.h"

#include <string>

#include "plugins/core.data/plugin.h"

namespace {

std::string read_asset_path(JNIEnv* env, jstring value, bool* valid) {
    if (value == nullptr) {
        *valid = false;
        return {};
    }
    const char* path = env->GetStringUTFChars(value, nullptr);
    if (path == nullptr) {
        *valid = false;
        return {};
    }
    std::string result(path);
    env->ReleaseStringUTFChars(value, path);
    return result;
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeCoreDataBegin(JNIEnv*, jclass, jint file_count,
                                                      jint total_bytes) {
    if (file_count < 0 || total_bytes < 0) return JNI_FALSE;
    return qol::contracts::succeeded(qol::features::core_data::begin_asset_import(
               static_cast<uint32_t>(file_count), static_cast<uint32_t>(total_bytes)))
               ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeCoreDataAsset(JNIEnv* env, jclass, jstring path,
                                                      jbyteArray bytes) {
    bool valid = true;
    const std::string path_value = read_asset_path(env, path, &valid);
    if (!valid || bytes == nullptr) return JNI_FALSE;
    const jsize size = env->GetArrayLength(bytes);
    if (size < 0) return JNI_FALSE;
    jbyte* data = env->GetByteArrayElements(bytes, nullptr);
    if (data == nullptr) return JNI_FALSE;
    const QolResult imported = qol::features::core_data::import_asset(
        path_value.data(), static_cast<uint32_t>(path_value.size()),
        reinterpret_cast<const uint8_t*>(data), static_cast<uint32_t>(size));
    env->ReleaseByteArrayElements(bytes, data, JNI_ABORT);
    return qol::contracts::succeeded(imported) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeCoreDataFinish(JNIEnv*, jclass) {
    return qol::contracts::succeeded(qol::features::core_data::finish_asset_import())
               ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeCoreDataCancel(JNIEnv*, jclass) {
    qol::features::core_data::cancel_asset_import();
}
