#include "gamebridge_internal.h"

// ---- 扩展背包正式操作面（control-plane §4.1 / ADR-008）----
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExtensionBagStatusJson(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(data_op_extension_bag_status_json().c_str());
#endif
}
