#include "gamebridge_internal.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveSlotsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_save_slots_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpNpcInteract(JNIEnv* env, jclass) {
    return op_result(env, "op_npc_interact", (std::string("")), data_op_npc_interact());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeShopItems(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_shop_items_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpShopBuy(JNIEnv* env, jclass, jint slot) {
    return op_result(env, "op_shop_buy", ("slot=" + str_of(slot)), data_op_shop_buy(static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeNpcDialogOptions(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_npc_dialog_options_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeDialogContent(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_dialog_content_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDialogSelect(JNIEnv* env, jclass, jstring action, jint index) {
    const char* a = action != nullptr ? env->GetStringUTFChars(action, nullptr) : nullptr;
    std::string s = a != nullptr ? a : "";
    if (a != nullptr) env->ReleaseStringUTFChars(action, a);
    return op_result(env, "op_dialog_select", ("s=" + str_of(s) + " " + "index=" + str_of(index)), data_op_dialog_select(s, static_cast<int>(index)));
}

