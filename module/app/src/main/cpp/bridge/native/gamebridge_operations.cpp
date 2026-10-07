#include "gamebridge_internal.h"

#include "bridge/native/character_write_result.h"
#include "bridge/native/item_write_result.h"
#include "game/access/quest.h"
#include "game/access/ui_state.h"
#include "game/access/save.h"
#include "game/access/save_transaction.h"
#include "game/adapter_ports/world_port.h"
#include "game/ops/character_write.h"
#include "game/ops/game_ops_common.h"
#include "game/ops/item_money.h"
#include "infra/store.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetMoney(JNIEnv* env, jclass, jlong money) {
#if QOL_KERNEL_ONLY
    (void)money;
    return kernel_only_result(env);
#else
    const QolResult result = qol::game::ops::set_money(static_cast<int64_t>(money));
    return op_result(env, "op_set_money", ("money=" + str_of(money)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAddMoney(JNIEnv* env, jclass, jlong delta) {
#if QOL_KERNEL_ONLY
    (void)delta;
    return kernel_only_result(env);
#else
    const QolResult result = qol::game::ops::add_money(static_cast<int64_t>(delta));
    return op_result(env, "op_add_money", ("delta=" + str_of(delta)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpMinusMoney(JNIEnv* env, jclass, jlong delta) {
#if QOL_KERNEL_ONLY
    (void)delta;
    return kernel_only_result(env);
#else
    const QolResult result = qol::game::ops::minus_money(static_cast<int64_t>(delta));
    return op_result(env, "op_minus_money", ("delta=" + str_of(delta)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetExperience(JNIEnv* env, jclass, jint role, jlong exp) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result =
        qol::game::ops::set_experience(static_cast<int>(role), static_cast<int64_t>(exp), &op);
    return op_result(env, "op_set_experience", ("role=" + str_of(role) + " " + "exp=" + str_of(exp)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetLevel(JNIEnv* env, jclass, jint role, jint level, jboolean force) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::set_level(static_cast<int>(role),
                                                       static_cast<int32_t>(level),
                                                       force == JNI_TRUE ? 1 : 0, &op);
    return op_result(env, "op_set_level", ("role=" + str_of(role) + " " + "level=" + str_of(level) + " " + "force=" + str_of(force == JNI_TRUE)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAddExperience(JNIEnv* env, jclass, jint role, jlong delta) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::add_experience(static_cast<int>(role),
                                                            static_cast<int64_t>(delta), &op);
    return op_result(env, "op_add_experience", ("role=" + str_of(role) + " " + "delta=" + str_of(delta)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetStatusPoint(JNIEnv* env, jclass, jint role, jint points) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result =
        qol::game::ops::set_status_point(static_cast<int>(role), static_cast<int32_t>(points), &op);
    return op_result(env, "op_set_status_point", ("role=" + str_of(role) + " " + "points=" + str_of(points)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAddStat(JNIEnv* env, jclass, jint role, jint attr) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result =
        qol::game::ops::add_stat(static_cast<int>(role), static_cast<int32_t>(attr), &op);
    return op_result(env, "op_add_stat", ("role=" + str_of(role) + " " + "attr=" + str_of(attr)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpStatReset(JNIEnv* env, jclass, jint role) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::stat_reset(static_cast<int>(role), &op);
    return op_result(env, "op_stat_reset", ("role=" + str_of(role)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSkillReset(JNIEnv* env, jclass, jint role) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::skill_reset(static_cast<int>(role), &op);
    return op_result(env, "op_skill_reset", ("role=" + str_of(role)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpCast(JNIEnv* env, jclass, jint role, jint actionId) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::cast_skill(static_cast<int>(role),
                                                        static_cast<int32_t>(actionId), &op);
    return op_result(env, "op_cast", ("role=" + str_of(role) + " " + "actionId=" + str_of(actionId)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpQuestQuit(JNIEnv* env, jclass, jint questId) {
    const qol::game::QuestQuitResult quit = qol::game::quest_quit(static_cast<int32_t>(questId));
    if (!quit.ok) {
        return op_result(env, "op_quest_quit", ("questId=" + str_of(questId)),
                         ::op_err(quit.error));
    }
    // 成功：op_ok() 同步力刷缓存并返回 {"ok":true}（与迁移前 quest quit 全局逐字一致）。
    return op_result(env, "op_quest_quit", ("questId=" + str_of(questId)),
                     ::op_ok());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSave(JNIEnv* env, jclass) {
    return op_result(env, "op_save", (std::string("")), qol::game::save_game().str());
}

// ---- 存档管理器备份（save-backup feature：bundle 导出/导入/列表/删除收口 native）----
extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveBackupInit(JNIEnv* env, jclass, jstring dataDir, jstring externalDir) {
#if QOL_KERNEL_ONLY
    (void)env;
    (void)dataDir;
    (void)externalDir;
#else
    const char* d = dataDir != nullptr ? env->GetStringUTFChars(dataDir, nullptr) : nullptr;
    const char* e = externalDir != nullptr ? env->GetStringUTFChars(externalDir, nullptr) : nullptr;
    save_backup_init(d, e);
    // 保存事务 Adapter 的平台路径上下文：复用既有 dataDir 注入点，不新增 NativeBridge external。
    qol::game::save_transaction_set_platform_dir(d);
    // B4-1 过渡：infra/store 沿用同一 externalFilesDir 注入点（B4-2/B4-3 收敛为
    // 独立 store 初始化边界）；此处仅初始化容器层，不切换任何消费者。
    qol::infra::store::init(e);
    if (d != nullptr) env->ReleaseStringUTFChars(dataDir, d);
    if (e != nullptr) env->ReleaseStringUTFChars(externalDir, e);
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSaveBackupSetMapNames(JNIEnv* env, jclass, jstring json) {
#if QOL_KERNEL_ONLY
    (void)env;
    (void)json;
#else
    if (json == nullptr) {
        save_backup_set_map_names(nullptr);
        return;
    }
    const char* c = env->GetStringUTFChars(json, nullptr);
    save_backup_set_map_names(c);
    env->ReleaseStringUTFChars(json, c);
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetWorldMapExits(JNIEnv* env, jclass, jstring json) {
#if QOL_KERNEL_ONLY
    (void)env;
    (void)json;
#else
    if (json == nullptr) {
        qol::game::world_port_set_map_exits_json(nullptr);
        return;
    }
    const char* value = env->GetStringUTFChars(json, nullptr);
    qol::game::world_port_set_map_exits_json(value);
    env->ReleaseStringUTFChars(json, value);
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeBackupList(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return op_result(env, "backup_list", (std::string("")), save_backup_list_json());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeBackupExport(JNIEnv* env, jclass, jint slot) {
#if QOL_KERNEL_ONLY
    (void)slot;
    return kernel_only_result(env);
#else
    return op_result(env, "backup_export", ("slot=" + str_of(slot)),
                     save_backup_export_json(static_cast<int>(slot)));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeBackupImport(JNIEnv* env, jclass, jstring checksum, jint slot) {
#if QOL_KERNEL_ONLY
    (void)checksum;
    (void)slot;
    return kernel_only_result(env);
#else
    const char* c = checksum != nullptr ? env->GetStringUTFChars(checksum, nullptr) : nullptr;
    std::string s = c != nullptr ? c : "";
    if (c != nullptr) env->ReleaseStringUTFChars(checksum, c);
    return op_result(env, "backup_import", ("checksum=" + s + " slot=" + str_of(slot)),
                     save_backup_import_json(s.c_str(), static_cast<int>(slot)));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeBackupDelete(JNIEnv* env, jclass, jstring checksum) {
#if QOL_KERNEL_ONLY
    (void)checksum;
    return kernel_only_result(env);
#else
    const char* c = checksum != nullptr ? env->GetStringUTFChars(checksum, nullptr) : nullptr;
    std::string s = c != nullptr ? c : "";
    if (c != nullptr) env->ReleaseStringUTFChars(checksum, c);
    return op_result(env, "backup_delete", ("checksum=" + s), save_backup_delete_json(s.c_str()));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpMainMenu(JNIEnv* env, jclass) {
    return op_result(env, "op_main_menu", (std::string("")), qol::game::ui_main_menu(false).str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpEnterSlot(JNIEnv* env, jclass, jint slot) {
    return op_result(env, "op_enter_slot", ("slot=" + str_of(slot)), qol::game::enter_slot(static_cast<int32_t>(slot)).str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpCreateSlot(JNIEnv* env, jclass, jint slot, jint classIdx) {
    return op_result(env, "op_create_slot", ("slot=" + str_of(slot) + " " + "classIdx=" + str_of(classIdx)), qol::game::create_slot(static_cast<int32_t>(slot), static_cast<int32_t>(classIdx)).str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpPanelClose(JNIEnv* env, jclass) {
    return op_result(env, "op_panel_close", (std::string("")), qol::game::ui_panel_close().str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpPanelOpen(JNIEnv* env, jclass, jstring panel) {
    const char* p = panel != nullptr ? env->GetStringUTFChars(panel, nullptr) : nullptr;
    std::string s = p != nullptr ? p : "";
    if (p != nullptr) env->ReleaseStringUTFChars(panel, p);
    return op_result(env, "op_panel_open", ("s=" + str_of(s)), qol::game::ui_panel_open(s).str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRecoverAfterHiveBlock(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_recover_after_hive_block().c_str());
}
