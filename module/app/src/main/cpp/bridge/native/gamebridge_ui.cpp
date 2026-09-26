#include "gamebridge_internal.h"

#include "bridge/native/item_write_result.h"
#include "game/access/save.h"
#include "game/access/shop.h"
#include "game/access/ui_state.h"
#include "game/ops/item_inventory_state_write.h"


extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveSlotsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::game::save_slots_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpNpcInteract(JNIEnv* env, jclass) {
    return op_result(env, "op_npc_interact", (std::string("")), qol::game::npc_interact().str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeShopItems(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::game::shop_items_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpShopBuy(JNIEnv* env, jclass, jint slot) {
    // shop_buy 无 kernel stub（game 层 shop 读无条件编译）：维持真实执行，不加 kernel 门。
    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result = qol::game::ops::shop_buy(static_cast<int>(slot), &out);
    return op_result(env, "op_shop_buy", ("slot=" + str_of(slot)),
                     qol::bridge::item_shop_buy_result_json(result, out));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeNpcDialogOptions(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::game::npc_dialog_options_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeDialogContent(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::game::dialog_content_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDialogSelect(JNIEnv* env, jclass, jstring action, jint index) {
    const char* a = action != nullptr ? env->GetStringUTFChars(action, nullptr) : nullptr;
    std::string s = a != nullptr ? a : "";
    if (a != nullptr) env->ReleaseStringUTFChars(action, a);
    return op_result(env, "op_dialog_select", ("s=" + str_of(s) + " " + "index=" + str_of(index)), qol::game::dialog_select(s, static_cast<int>(index)).str());
}

