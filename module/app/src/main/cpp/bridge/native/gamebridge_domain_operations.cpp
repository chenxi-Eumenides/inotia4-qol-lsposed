#include "gamebridge_internal.h"

#include "bridge/native/character_write_result.h"
#include "bridge/native/item_write_result.h"
#include "contracts/commands.h"
#include "game/ops/character_write.h"
#include "game/ops/item_inventory_state_write.h"

// HTTP 写路由只表达绝对值与既有动词；单技能档位与按类别删除没有等价路由，
// 因此保留这两个 JNI 入口。nativeGetEventsJson 仍是 /api/system/events 的读面。
extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetSkillUsage(JNIEnv* env, jclass, jint role, jint onoff) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::set_skill_usage(static_cast<int>(role),
                                                             static_cast<int32_t>(onoff), &op);
    return op_result(env, "op_set_skill_usage", ("role=" + str_of(role) + " " + "onoff=" + str_of(onoff)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpRemoveItem(JNIEnv* env, jclass, jint category) {
#if QOL_KERNEL_ONLY
    (void)category;
    return kernel_only_result(env);
#else
    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result =
        qol::game::ops::remove_item(static_cast<int32_t>(category), &out);
    return op_result(env, "op_remove_item", ("category=" + str_of(category)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetEventsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_events_json().c_str());
}
