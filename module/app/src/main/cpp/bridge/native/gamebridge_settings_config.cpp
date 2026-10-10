// Frozen compatibility exports for the retired no-owner Kotlin settings writer.
#include <jni.h>

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsConfigInit(JNIEnv*, jclass, jclass, jintArray,
                                                           jlong) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsConfigPublish(JNIEnv*, jclass, jintArray, jlong) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSettingsConfigComplete(JNIEnv*, jclass, jlong, jint,
                                                               jint, jint, jint, jstring, jstring) {
    return JNI_FALSE;
}
