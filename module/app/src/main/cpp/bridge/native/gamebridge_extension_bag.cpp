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

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpExtensionBagEnterView(JNIEnv* env, jclass, jint bag) {
#if QOL_KERNEL_ONLY
    (void)bag;
    return kernel_only_result(env);
#else
    return op_result(env, "op_extension_bag_enter_view", ("bag=" + str_of(bag)),
                     data_op_extension_bag_enter_view(static_cast<int>(bag)));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpExtensionBagUnequip(JNIEnv* env, jclass, jint bag) {
#if QOL_KERNEL_ONLY
    (void)bag;
    return kernel_only_result(env);
#else
    return op_result(env, "op_extension_bag_unequip", ("bag=" + str_of(bag)),
                     data_op_extension_bag_unequip(static_cast<int>(bag)));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpExtensionBagExitView(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return op_result(env, "op_extension_bag_exit_view", (std::string("")),
                     data_op_extension_bag_exit_view());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpExtensionBagSelectBag(JNIEnv* env, jclass, jint bag) {
#if QOL_KERNEL_ONLY
    (void)bag;
    return kernel_only_result(env);
#else
    return op_result(env, "op_extension_bag_select_bag", ("bag=" + str_of(bag)),
                     data_op_extension_bag_select_bag(static_cast<int>(bag)));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpExtensionBagClickItem(JNIEnv* env, jclass, jint bag, jint slot) {
#if QOL_KERNEL_ONLY
    (void)bag;
    (void)slot;
    return kernel_only_result(env);
#else
    return op_result(env, "op_extension_bag_click_item", ("bag=" + str_of(bag) + " slot=" + str_of(slot)),
                     data_op_extension_bag_click_item(static_cast<int>(bag), static_cast<int>(slot)));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpExtensionBagMoveItem(JNIEnv* env, jclass, jint from_bag, jint from_slot,
                                                                    jint to_bag, jint to_slot) {
#if QOL_KERNEL_ONLY
    (void)from_bag;
    (void)from_slot;
    (void)to_bag;
    (void)to_slot;
    return kernel_only_result(env);
#else
    return op_result(env, "op_extension_bag_move_item",
                     ("from_bag=" + str_of(from_bag) + " from_slot=" + str_of(from_slot) +
                      " to_bag=" + str_of(to_bag) + " to_slot=" + str_of(to_slot)),
                     data_op_extension_bag_move_item(static_cast<int>(from_bag), static_cast<int>(from_slot),
                                                       static_cast<int>(to_bag), static_cast<int>(to_slot)));
#endif
}
