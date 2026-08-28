#include <jni.h>

#include <string>

#include "game_access.h"
#include "game_cache.h"
#include "game_character.h"
#include "game_dialog.h"
#include "game_inventory.h"
#include "game_party.h"
#include "game_quest.h"
#include "game_save.h"
#include "game_shop.h"
#include "game_state.h"
#include "game_system.h"
#include "game_ui.h"
#include "game_world.h"
#include "game_tiles.h"
#include "game_patch.h"
#include "game_ui_exp.h"
#include "game_ui_settings.h"
#include "game_ui_virtbag.h"
#include <android/log.h>

#define MOVE_TAG "Inotia4Move"
#define MOVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, MOVE_TAG, __VA_ARGS__)

namespace {
JavaVM* g_cached_jvm = nullptr;
}  // namespace

JavaVM* g_jvm() { return g_cached_jvm; }

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_cached_jvm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_export_NativeBridge_nativeRegisterConfigBridge(JNIEnv* env, jclass, jclass bridge_class) {
    settings_register_config_bridge(env, bridge_class);
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_export_NativeBridge_nativeRegisterExtensionBagUiBridge(JNIEnv* env, jclass, jclass bridge_class) {
    virtual_bag_ui_register_bridge(env, bridge_class);
}

template <typename T>
inline std::string str_of(T v) { return std::to_string(static_cast<long long>(v)); }
inline std::string str_of(const std::string& v) { return v; }

jstring op_result(JNIEnv* env, const char* op, const std::string& argstr, const std::string& result) {
    MOVE_LOG("[%s] args={%s} -> %s", op, argstr.c_str(), result.c_str());
    return env->NewStringUTF(result.c_str());
}


// JNI 导出层：仅做参数传递与字符串转换，逻辑在 game_access / game_data。

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_export_NativeBridge_nativeInit(JNIEnv*, jclass) {
    bool ok = bridge_init();
    if (ok) {
        frame_cache_start();   // v0.4.59：存在 interval>0 槽时启动预取线程（自 game_access 移入）
        settings_ui_start_auto_inject();
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetBaseAddr(JNIEnv*, jclass) {
    return static_cast<jlong>(g_base);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetFrameCount(JNIEnv*, jclass) {
    return static_cast<jlong>(data_frame_count());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetActiveQuest(JNIEnv*, jclass) {
    return data_active_quest();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeQuestList(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_quest_list_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeQuestCompleted(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_quest_completed_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeQuestActive(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_quest_active_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeCurrentSaveSlot(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_current_save_slot_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetInitReport(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_init_report().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetPlayerJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_player_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetPartyJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_party_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetInventoryJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_inventory_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetMapJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_map_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetTilesJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(build_tiles_json().c_str());
}

// P0#瓦片矩阵（2026-08-12）：Kotlin 读取 assets maps/tiles.json 后传入，native 解析缓存
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_export_NativeBridge_nativeSetTilesData(JNIEnv* env, jclass, jstring json) {
    const char* j = json != nullptr ? env->GetStringUTFChars(json, nullptr) : nullptr;
    if (j == nullptr) return JNI_FALSE;
    set_static_tiles(std::string(j));
    env->ReleaseStringUTFChars(json, j);
    return static_tiles_ready() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetUnitsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_units_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetEnemiesJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_enemies_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetInteractivesJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_interactives_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetGamestateJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_gamestate_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetDebugUiJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_debug_ui_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetSnapshotJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_snapshot_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetSkillsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_skills_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetMercenariesJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_mercenaries_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetDropsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_drops_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetPathJson(JNIEnv* env, jclass, jint tx, jint ty) {
    return env->NewStringUTF(data_path_json(static_cast<int>(tx), static_cast<int>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeDistanceJson(JNIEnv* env, jclass, jint tx, jint ty) {
    return env->NewStringUTF(data_distance_json(static_cast<int32_t>(tx), static_cast<int32_t>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeDebugPathJson(JNIEnv* env, jclass, jint tx, jint ty) {
    return env->NewStringUTF(data_debug_path_json(static_cast<int32_t>(tx), static_cast<int32_t>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetMoney(JNIEnv* env, jclass, jlong money) {
    return op_result(env, "op_set_money", ("money=" + str_of(money)), data_op_set_money(static_cast<int64_t>(money)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpAddMoney(JNIEnv* env, jclass, jlong delta) {
    return op_result(env, "op_add_money", ("delta=" + str_of(delta)), data_op_add_money(static_cast<int64_t>(delta)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpMinusMoney(JNIEnv* env, jclass, jlong delta) {
    return op_result(env, "op_minus_money", ("delta=" + str_of(delta)), data_op_minus_money(static_cast<int64_t>(delta)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetExperience(JNIEnv* env, jclass, jint role, jlong exp) {
    return op_result(env, "op_set_experience", ("role=" + str_of(role) + " " + "exp=" + str_of(exp)), data_op_set_experience(static_cast<int>(role), static_cast<int64_t>(exp)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetLevel(JNIEnv* env, jclass, jint role, jint level, jboolean force) {
    return op_result(env, "op_set_level", ("role=" + str_of(role) + " " + "level=" + str_of(level) + " " + "force=" + str_of(force == JNI_TRUE)),
                     data_op_set_level(static_cast<int>(role), static_cast<int32_t>(level), force == JNI_TRUE));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpAddExperience(JNIEnv* env, jclass, jint role, jlong delta) {
    return op_result(env, "op_add_experience", ("role=" + str_of(role) + " " + "delta=" + str_of(delta)), data_op_add_experience(static_cast<int>(role), static_cast<int64_t>(delta)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetStatusPoint(JNIEnv* env, jclass, jint role, jint points) {
    return op_result(env, "op_set_status_point", ("role=" + str_of(role) + " " + "points=" + str_of(points)), data_op_set_status_point(static_cast<int>(role), static_cast<int32_t>(points)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpAddStat(JNIEnv* env, jclass, jint role, jint attr) {
    return op_result(env, "op_add_stat", ("role=" + str_of(role) + " " + "attr=" + str_of(attr)), data_op_add_stat(static_cast<int>(role), static_cast<int32_t>(attr)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpStatReset(JNIEnv* env, jclass, jint role) {
    return op_result(env, "op_stat_reset", ("role=" + str_of(role)), data_op_stat_reset(static_cast<int>(role)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSkillReset(JNIEnv* env, jclass, jint role) {
    return op_result(env, "op_skill_reset", ("role=" + str_of(role)), data_op_skill_reset(static_cast<int>(role)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpCast(JNIEnv* env, jclass, jint role, jint actionId) {
    return op_result(env, "op_cast", ("role=" + str_of(role) + " " + "actionId=" + str_of(actionId)), data_op_cast(static_cast<int>(role), static_cast<int32_t>(actionId)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpQuestQuit(JNIEnv* env, jclass, jint questId) {
    return op_result(env, "op_quest_quit", ("questId=" + str_of(questId)), data_op_quest_quit(static_cast<int32_t>(questId)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSave(JNIEnv* env, jclass) {
    return op_result(env, "op_save", (std::string("")), data_op_save());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpMainMenu(JNIEnv* env, jclass) {
    return op_result(env, "op_main_menu", (std::string("")), data_op_main_menu());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpEnterSlot(JNIEnv* env, jclass, jint slot) {
    return op_result(env, "op_enter_slot", ("slot=" + str_of(slot)), data_op_enter_slot(static_cast<int32_t>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpCreateSlot(JNIEnv* env, jclass, jint slot, jint classIdx) {
    return op_result(env, "op_create_slot", ("slot=" + str_of(slot) + " " + "classIdx=" + str_of(classIdx)), data_op_create_slot(static_cast<int32_t>(slot), static_cast<int32_t>(classIdx)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpPanelClose(JNIEnv* env, jclass) {
    return op_result(env, "op_panel_close", (std::string("")), data_op_panel_close());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpPanelOpen(JNIEnv* env, jclass, jstring panel) {
    const char* p = panel != nullptr ? env->GetStringUTFChars(panel, nullptr) : nullptr;
    std::string s = p != nullptr ? p : "";
    if (p != nullptr) env->ReleaseStringUTFChars(panel, p);
    return op_result(env, "op_panel_open", ("s=" + str_of(s)), data_op_panel_open(s));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeRecoverAfterHiveBlock(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_recover_after_hive_block().c_str());
}

// ---- UI 实验端点（ui-exp v0.6.7）----
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExp1BtnBehavior(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp1_btn_behavior().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExp2AddControl(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp2_add_control().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExp3CustomDialog(JNIEnv* env, jclass, jstring text) {
    const char* t = text != nullptr ? env->GetStringUTFChars(text, nullptr) : nullptr;
    std::string s = t != nullptr ? t : "EXP3 dialog";
    if (t != nullptr) env->ReleaseStringUTFChars(text, t);
    return env->NewStringUTF(data_exp3_custom_dialog(s).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExp4TextAppearance(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp4_text_appearance().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExp5NewPanel(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp5_new_panel().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExpRestoreAll(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp_restore_all().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExpStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_exp_status_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeSaveSlotsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_save_slots_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpNpcInteract(JNIEnv* env, jclass) {
    return op_result(env, "op_npc_interact", (std::string("")), data_op_npc_interact());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeShopItems(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_shop_items_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpShopBuy(JNIEnv* env, jclass, jint slot) {
    return op_result(env, "op_shop_buy", ("slot=" + str_of(slot)), data_op_shop_buy(static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeNpcDialogOptions(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_npc_dialog_options_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeDialogContent(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_dialog_content_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpDialogSelect(JNIEnv* env, jclass, jstring action, jint index) {
    const char* a = action != nullptr ? env->GetStringUTFChars(action, nullptr) : nullptr;
    std::string s = a != nullptr ? a : "";
    if (a != nullptr) env->ReleaseStringUTFChars(action, a);
    return op_result(env, "op_dialog_select", ("s=" + str_of(s) + " " + "index=" + str_of(index)), data_op_dialog_select(s, static_cast<int>(index)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpJewel(JNIEnv* env, jclass, jint role, jint bag, jint slot, jint equipSlot) {
    return op_result(env, "op_jewel", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "equipSlot=" + str_of(equipSlot)), data_op_jewel(static_cast<int>(role), static_cast<int>(bag),
        static_cast<int>(slot), static_cast<int>(equipSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpEnchant(JNIEnv* env, jclass, jint role, jint bag, jint slot, jint equipSlot) {
    return op_result(env, "op_enchant", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "equipSlot=" + str_of(equipSlot)), data_op_enchant(static_cast<int>(role), static_cast<int>(bag),
        static_cast<int>(slot), static_cast<int>(equipSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetAutoAttack(JNIEnv* env, jclass, jint role, jint onoff) {
    return op_result(env, "op_set_auto_attack", ("role=" + str_of(role) + " " + "onoff=" + str_of(onoff)), data_op_set_auto_attack(static_cast<int>(role), static_cast<int32_t>(onoff)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetSkillUsage(JNIEnv* env, jclass, jint role, jint onoff) {
    return op_result(env, "op_set_skill_usage", ("role=" + str_of(role) + " " + "onoff=" + str_of(onoff)), data_op_set_skill_usage(static_cast<int>(role), static_cast<int32_t>(onoff)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpEquip(JNIEnv* env, jclass, jint role, jint bag, jint slot) {
    return op_result(env, "op_equip", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot)), data_op_equip(static_cast<int>(role), static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpUnequip(JNIEnv* env, jclass, jint role, jint slot) {
    return op_result(env, "op_unequip", ("role=" + str_of(role) + " " + "slot=" + str_of(slot)), data_op_unequip(static_cast<int>(role), static_cast<int32_t>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSwitchPlayer(JNIEnv* env, jclass, jint slot) {
    return op_result(env, "op_switch_player", ("slot=" + str_of(slot)), data_op_switch_player(static_cast<int32_t>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpPartySwap(JNIEnv* env, jclass, jint a, jint b) {
    return op_result(env, "op_party_swap", ("a=" + str_of(a) + " " + "b=" + str_of(b)), data_op_party_swap(static_cast<int32_t>(a), static_cast<int32_t>(b)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpTeleport(JNIEnv* env, jclass, jint mapId, jint x, jint y) {
    return op_result(env, "op_teleport", ("mapId=" + str_of(mapId) + " " + "x=" + str_of(x) + " " + "y=" + str_of(y)), data_op_teleport(static_cast<int32_t>(mapId), static_cast<int32_t>(x), static_cast<int32_t>(y)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpRemoveItem(JNIEnv* env, jclass, jint category) {
    return op_result(env, "op_remove_item", ("category=" + str_of(category)), data_op_remove_item(static_cast<int32_t>(category)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpLearnAction(JNIEnv* env, jclass, jint role, jint actionId, jint level) {
    return op_result(env, "op_learn_action", ("role=" + str_of(role) + " " + "actionId=" + str_of(actionId) + " " + "level=" + str_of(level)), data_op_learn_action(static_cast<int>(role), static_cast<int32_t>(actionId), static_cast<int32_t>(level)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeGetEventsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_events_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpMove(JNIEnv* env, jclass, jint x, jint y) {
    return op_result(env, "op_move", ("x=" + str_of(x) + " " + "y=" + str_of(y)), data_op_move(static_cast<int32_t>(x), static_cast<int32_t>(y)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpWalk(JNIEnv* env, jclass, jint direction) {
    return op_result(env, "op_walk", ("direction=" + str_of(direction)), data_op_walk(static_cast<int32_t>(direction)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpWalkStop(JNIEnv* env, jclass) {
    return op_result(env, "op_walk_stop", (std::string("")), data_op_walk_stop());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpInteract(JNIEnv* env, jclass) {
    return op_result(env, "op_interact", (std::string("")), data_op_interact());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpAttack(JNIEnv* env, jclass, jint role, jint target_slot) {
    return op_result(env, "op_attack", ("role=" + str_of(role) + " " + "target_slot=" + str_of(target_slot)), data_op_attack(static_cast<int32_t>(role), static_cast<int32_t>(target_slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpStopCombat(JNIEnv* env, jclass, jint role) {
    return op_result(env, "op_stop_combat", ("role=" + str_of(role)), data_op_stop_combat(static_cast<int32_t>(role)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpUseItem(JNIEnv* env, jclass, jint bag, jint slot) {
    return op_result(env, "op_use_item", ("bag=" + str_of(bag) + " " + "slot=" + str_of(slot)), data_op_use_item(static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpDiceAccept(JNIEnv* env, jclass) {
    return op_result(env, "op_dice_accept", (std::string("")), data_op_dice_accept());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpDiceReject(JNIEnv* env, jclass) {
    return op_result(env, "op_dice_reject", (std::string("")), data_op_dice_reject());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpDiscardItem(JNIEnv* env, jclass, jint bag, jint slot) {
    return op_result(env, "op_discard_item", ("bag=" + str_of(bag) + " " + "slot=" + str_of(slot)), data_op_discard_item(static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSellItem(JNIEnv* env, jclass, jint bag, jint slot) {
    return op_result(env, "op_sell_item", ("bag=" + str_of(bag) + " " + "slot=" + str_of(slot)), data_op_sell_item(static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpMoveItem(JNIEnv* env, jclass, jint bag, jint slot, jint count, jint toBag, jint toSlot) {
    return op_result(env, "op_move_item", ("bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "count=" + str_of(count) + " " + "toBag=" + str_of(toBag) + " " + "toSlot=" + str_of(toSlot)), data_op_move_item(static_cast<int>(bag), static_cast<int>(slot),
        static_cast<int>(count), static_cast<int>(toBag), static_cast<int>(toSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpIncludeParty(JNIEnv* env, jclass, jint mercSlot) {
    return op_result(env, "op_include_party", ("mercSlot=" + str_of(mercSlot)), data_op_include_party(static_cast<int>(mercSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpExcludeParty(JNIEnv* env, jclass, jint mercSlot) {
    return op_result(env, "op_exclude_party", ("mercSlot=" + str_of(mercSlot)), data_op_exclude_party(static_cast<int>(mercSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpDischarge(JNIEnv* env, jclass, jint mercSlot) {
    return op_result(env, "op_discharge", ("mercSlot=" + str_of(mercSlot)), data_op_discharge(static_cast<int>(mercSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpWithdraw(JNIEnv* env, jclass, jint mercSlot, jint equipSlot) {
    return op_result(env, "op_withdraw", ("mercSlot=" + str_of(mercSlot) + " " + "equipSlot=" + str_of(equipSlot)), data_op_withdraw(static_cast<int>(mercSlot), static_cast<int32_t>(equipSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetHp(JNIEnv* env, jclass, jint role, jint hp) {
    return op_result(env, "op_set_hp", ("role=" + str_of(role) + " " + "hp=" + str_of(hp)), data_op_set_hp(static_cast<int>(role), static_cast<int32_t>(hp)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetMp(JNIEnv* env, jclass, jint role, jint mp) {
    return op_result(env, "op_set_mp", ("role=" + str_of(role) + " " + "mp=" + str_of(mp)), data_op_set_mp(static_cast<int>(role), static_cast<int32_t>(mp)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpSetAttr(JNIEnv* env, jclass, jint role, jint attrIndex, jint value) {
    return op_result(env, "op_set_attr", ("role=" + str_of(role) + " " + "attrIndex=" + str_of(attrIndex) + " " + "value=" + str_of(value)), data_op_set_attr(static_cast<int>(role), static_cast<int32_t>(attrIndex), static_cast<int32_t>(value)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpAddItem(JNIEnv* env, jclass, jint category, jint count) {
    return op_result(env, "op_add_item", ("category=" + str_of(category) + " " + "count=" + str_of(count)), data_op_add_item(static_cast<int32_t>(category), static_cast<int32_t>(count)));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_export_NativeBridge_nativeSetStackLimitEnabled(JNIEnv*, jclass, jboolean enabled) {
    return set_stack_limit_enabled(enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_export_NativeBridge_nativeSetMoveMergeEnabled(JNIEnv*, jclass, jboolean enabled) {
    return set_move_merge_enabled(enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_export_NativeBridge_nativeSetExtensionBagEnabled(JNIEnv*, jclass, jboolean enabled) {
    return set_virtual_bag_enabled(enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

// ---- 模块设置 UI 端点（ui-settings v0.6.9）----
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeSettingsUiInject(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_inject().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeSettingsUiStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_status_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeSettingsUiRestore(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_restore().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeSettingsUiOpenOption(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_open_option().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeSettingsUiOpenPanel(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_settings_ui_open_panel().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExtensionBagUiStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_virtual_bag_ui_status_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExtensionBagTestEquip(JNIEnv* env, jclass, jint index, jint capacity) {
    return env->NewStringUTF(data_virtual_bag_test_equip(static_cast<int>(index), static_cast<int>(capacity)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExtensionBagTestItem(JNIEnv* env, jclass, jint index, jint slot,
                                                                jint category, jint count) {
    return env->NewStringUTF(data_virtual_bag_test_item(static_cast<int>(index), static_cast<int>(slot),
                                                        static_cast<int>(category), static_cast<int>(count)).c_str());
}

// ---- 扩展背包正式操作面（control-plane §4.1 / ADR-008）----
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeExtensionBagStatusJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_op_extension_bag_status_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpExtensionBagEnterView(JNIEnv* env, jclass, jint bag) {
    return op_result(env, "op_extension_bag_enter_view", ("bag=" + str_of(bag)),
                     data_op_extension_bag_enter_view(static_cast<int>(bag)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpExtensionBagExitView(JNIEnv* env, jclass) {
    return op_result(env, "op_extension_bag_exit_view", (std::string("")),
                     data_op_extension_bag_exit_view());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpExtensionBagSelectBag(JNIEnv* env, jclass, jint bag) {
    return op_result(env, "op_extension_bag_select_bag", ("bag=" + str_of(bag)),
                     data_op_extension_bag_select_bag(static_cast<int>(bag)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpExtensionBagClickItem(JNIEnv* env, jclass, jint bag, jint slot) {
    return op_result(env, "op_extension_bag_click_item", ("bag=" + str_of(bag) + " slot=" + str_of(slot)),
                     data_op_extension_bag_click_item(static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_export_NativeBridge_nativeOpExtensionBagMoveItem(JNIEnv* env, jclass, jint from_bag, jint from_slot,
                                                                  jint to_bag, jint to_slot) {
    return op_result(env, "op_extension_bag_move_item",
                     ("from_bag=" + str_of(from_bag) + " from_slot=" + str_of(from_slot) +
                      " to_bag=" + str_of(to_bag) + " to_slot=" + str_of(to_slot)),
                     data_op_extension_bag_move_item(static_cast<int>(from_bag), static_cast<int>(from_slot),
                                                     static_cast<int>(to_bag), static_cast<int>(to_slot)));
}
