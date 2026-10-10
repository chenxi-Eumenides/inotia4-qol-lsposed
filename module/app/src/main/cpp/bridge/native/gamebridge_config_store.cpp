// Path-only config bootstrap. Kotlin supplies the external files directory before
// NativeBridge init; assembly performs Store init/seed once all descriptors are registered.
#include "host/config_platform_bridge.h"

#include <jni.h>

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeConfigStoreBootstrap(JNIEnv* env, jclass, jstring directory) {
    if (directory == nullptr) return -1;
    const char* path = env->GetStringUTFChars(directory, nullptr);
    if (path == nullptr) return -1;
    const jint status = qol::host::config_store_bootstrap(path);
    env->ReleaseStringUTFChars(directory, path);
    return status;
}
