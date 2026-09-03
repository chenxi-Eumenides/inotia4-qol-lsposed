#include "gamebridge_internal.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetMoney(JNIEnv* env, jclass, jlong money) {
    return op_result(env, "op_set_money", ("money=" + str_of(money)), data_op_set_money(static_cast<int64_t>(money)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAddMoney(JNIEnv* env, jclass, jlong delta) {
    return op_result(env, "op_add_money", ("delta=" + str_of(delta)), data_op_add_money(static_cast<int64_t>(delta)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpMinusMoney(JNIEnv* env, jclass, jlong delta) {
    return op_result(env, "op_minus_money", ("delta=" + str_of(delta)), data_op_minus_money(static_cast<int64_t>(delta)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetExperience(JNIEnv* env, jclass, jint role, jlong exp) {
    return op_result(env, "op_set_experience", ("role=" + str_of(role) + " " + "exp=" + str_of(exp)), data_op_set_experience(static_cast<int>(role), static_cast<int64_t>(exp)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetLevel(JNIEnv* env, jclass, jint role, jint level, jboolean force) {
    return op_result(env, "op_set_level", ("role=" + str_of(role) + " " + "level=" + str_of(level) + " " + "force=" + str_of(force == JNI_TRUE)),
                     data_op_set_level(static_cast<int>(role), static_cast<int32_t>(level), force == JNI_TRUE));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAddExperience(JNIEnv* env, jclass, jint role, jlong delta) {
    return op_result(env, "op_add_experience", ("role=" + str_of(role) + " " + "delta=" + str_of(delta)), data_op_add_experience(static_cast<int>(role), static_cast<int64_t>(delta)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetStatusPoint(JNIEnv* env, jclass, jint role, jint points) {
    return op_result(env, "op_set_status_point", ("role=" + str_of(role) + " " + "points=" + str_of(points)), data_op_set_status_point(static_cast<int>(role), static_cast<int32_t>(points)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAddStat(JNIEnv* env, jclass, jint role, jint attr) {
    return op_result(env, "op_add_stat", ("role=" + str_of(role) + " " + "attr=" + str_of(attr)), data_op_add_stat(static_cast<int>(role), static_cast<int32_t>(attr)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpStatReset(JNIEnv* env, jclass, jint role) {
    return op_result(env, "op_stat_reset", ("role=" + str_of(role)), data_op_stat_reset(static_cast<int>(role)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSkillReset(JNIEnv* env, jclass, jint role) {
    return op_result(env, "op_skill_reset", ("role=" + str_of(role)), data_op_skill_reset(static_cast<int>(role)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpCast(JNIEnv* env, jclass, jint role, jint actionId) {
    return op_result(env, "op_cast", ("role=" + str_of(role) + " " + "actionId=" + str_of(actionId)), data_op_cast(static_cast<int>(role), static_cast<int32_t>(actionId)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpQuestQuit(JNIEnv* env, jclass, jint questId) {
    return op_result(env, "op_quest_quit", ("questId=" + str_of(questId)), data_op_quest_quit(static_cast<int32_t>(questId)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSave(JNIEnv* env, jclass) {
    return op_result(env, "op_save", (std::string("")), data_op_save());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpMainMenu(JNIEnv* env, jclass) {
    return op_result(env, "op_main_menu", (std::string("")), data_op_main_menu());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpEnterSlot(JNIEnv* env, jclass, jint slot) {
    return op_result(env, "op_enter_slot", ("slot=" + str_of(slot)), data_op_enter_slot(static_cast<int32_t>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpCreateSlot(JNIEnv* env, jclass, jint slot, jint classIdx) {
    return op_result(env, "op_create_slot", ("slot=" + str_of(slot) + " " + "classIdx=" + str_of(classIdx)), data_op_create_slot(static_cast<int32_t>(slot), static_cast<int32_t>(classIdx)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpPanelClose(JNIEnv* env, jclass) {
    return op_result(env, "op_panel_close", (std::string("")), data_op_panel_close());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpPanelOpen(JNIEnv* env, jclass, jstring panel) {
    const char* p = panel != nullptr ? env->GetStringUTFChars(panel, nullptr) : nullptr;
    std::string s = p != nullptr ? p : "";
    if (p != nullptr) env->ReleaseStringUTFChars(panel, p);
    return op_result(env, "op_panel_open", ("s=" + str_of(s)), data_op_panel_open(s));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRecoverAfterHiveBlock(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_recover_after_hive_block().c_str());
}

