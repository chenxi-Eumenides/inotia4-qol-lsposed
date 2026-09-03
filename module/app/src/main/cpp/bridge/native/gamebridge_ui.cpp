#include "gamebridge_internal.h"

// ---- UI 实验端点（ui-exp v0.6.7）----
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExp1BtnBehavior(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp1_btn_behavior().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExp2AddControl(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp2_add_control().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExp3CustomDialog(JNIEnv* env, jclass, jstring text) {
    const char* t = text != nullptr ? env->GetStringUTFChars(text, nullptr) : nullptr;
    std::string s = t != nullptr ? t : "EXP3 dialog";
    if (t != nullptr) env->ReleaseStringUTFChars(text, t);
    return env->NewStringUTF(data_exp3_custom_dialog(s).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExp4TextAppearance(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp4_text_appearance().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExp5NewPanel(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp5_new_panel().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExpRestoreAll(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp_restore_all().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeExpStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp_status_json().c_str());
}

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

