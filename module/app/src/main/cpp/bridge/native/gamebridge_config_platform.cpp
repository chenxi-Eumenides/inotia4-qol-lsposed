#include <jni.h>

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeConfigPlatformSubscribe(JNIEnv* env, jclass,
                                                                jclass callbacks) {
    (void)env;
    (void)callbacks;
    return JNI_FALSE;
}

// Kotlin reports a listener bind failure with the revision it observed. The native config
// owner rewrites listen_port to the default (8088) and emits a revision effect, which the
// platform projection delivers back so the server restarts on the fallback port. The export
// signature stays frozen; the second failure on 8088 is rejected to avoid a fallback loop.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeConfigPlatformReportBindFailure(JNIEnv*, jclass,
                                                                         jlong revision, jint failed_port) {
    (void)revision;
    (void)failed_port;
    return JNI_FALSE;
}
