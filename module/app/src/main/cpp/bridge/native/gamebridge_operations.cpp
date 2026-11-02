#include "gamebridge_internal.h"

#include "bridge/native/character_write_result.h"
#include "bridge/native/item_write_result.h"
#include "game/access/save_transaction.h"
#include "game/adapter_ports/world_port.h"
#include "game/ops/character_write.h"
#include "game/ops/item_money.h"
#include "infra/store.h"

// HTTP 写路由只表达绝对值（/api/op/inventory/money、/api/op/character/{role}/experience），
// 增量语义没有等价路由，因此保留这三个 JNI 入口。
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
Java_com_inotia4_qol_NativeBridge_nativeOpAddExperience(JNIEnv* env, jclass, jint role, jlong delta) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::add_experience(static_cast<int>(role),
                                                            static_cast<int64_t>(delta), &op);
    return op_result(env, "op_add_experience", ("role=" + str_of(role) + " " + "delta=" + str_of(delta)),
                     qol::bridge::character_write_result_json(result, op));
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
Java_com_inotia4_qol_NativeBridge_nativeRecoverAfterHiveBlock(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_recover_after_hive_block().c_str());
}
