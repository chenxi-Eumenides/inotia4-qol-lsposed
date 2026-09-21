// gamebridge_autosell.cpp —— 自动出售 JNI 薄层（阶段 B）。
//
// 仅做 JNI 参数/返回值转换；配置与状态逻辑在 plugins/autosell/*。

#include "gamebridge_internal.h"

#if !QOL_KERNEL_ONLY
#include "contracts/commands.h"
#include "plugins/autosell/config.h"
#include "plugins/autosell/scan.h"
#include "plugins/autosell/store.h"
#include "infra/commands.h"

namespace {

// M-11：native 侧入参钳制，与 Kotlin 校验一致（不可信输入按边界收敛，不越界驱动规则）。
int clamp_range(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

// 模块 sidecar 有效槽（ModuleSaveStore SLOT_COUNT=3）。
bool valid_persist_slot(int slot) { return slot >= 0 && slot < 3; }

}  // namespace
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellConfig(
    JNIEnv* env, jclass, jboolean enabled, jint rarity, jint enhance, jint socket, jint gemTier,
    jstring specialJson, jint gemRange) {
#if QOL_KERNEL_ONLY
    (void)env;
    (void)enabled;
    (void)rarity;
    (void)enhance;
    (void)socket;
    (void)gemTier;
    (void)specialJson;
    (void)gemRange;
    return JNI_FALSE;
#else
    autosell::Config cfg;
    // 值即开关：0=关闭，正整数为 1-based 档位；按边界钳制不可信输入。
    cfg.enabled = enabled == JNI_TRUE;
    cfg.rarity = clamp_range(static_cast<int>(rarity), 0, 5);
    cfg.enhance = clamp_range(static_cast<int>(enhance), 0, 32);
    cfg.socket = clamp_range(static_cast<int>(socket), 0, 16);
    cfg.gem_tier = clamp_range(static_cast<int>(gemTier), 0, 5);
    cfg.gem_range = clamp_range(static_cast<int>(gemRange), 0, 5);
    // 特殊类型入参为 JSON 数组文本（如 ["dice"]）；null/坏串 -> 空集（名字->位表在 plugins/autosell/store.h）。
    const char* special_chars =
        specialJson != nullptr ? env->GetStringUTFChars(specialJson, nullptr) : nullptr;
    cfg.special_mask = autosell_special_parse_array(special_chars);
    if (special_chars != nullptr) {
        env->ReleaseStringUTFChars(specialJson, special_chars);
    }
    // 写入运行时配置并按 enabled 注册 / 删除 60 帧周期扫描任务。
    autosell_apply_config(cfg);
    // 按存档持久化：仅当前存档槽合法时直写 sidecar `autosell` section（不进 journal）。
    const int slot = current_save_slot();
    if (valid_persist_slot(slot)) {
        autosell_store_persist(slot, cfg);
    }
    return JNI_TRUE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellEnabled(JNIEnv*, jclass, jboolean enabled) {
#if QOL_KERNEL_ONLY
    (void)enabled;
    return JNI_FALSE;
#else
    // 只做参数转换：开关下发为一条命令（kCommandSetConfigBool），由主线程在下一逻辑帧
    // 写配置；autosell 插件的 on_enabled/on_disabled 与配置事件据此同步扫描任务与 UI 门控。
    // 返回值表示「本次下发是否成功入队」，不是开关状态（与 nativeSetSimpleModeEnabled 同构）。
    const qol::contracts::QolCommand command{
        qol::contracts::kCommandSetConfigBool,
        "plugin.autosell.enabled",
        enabled == JNI_TRUE ? 1 : 0,
    };
    qol::contracts::QolCommandTicket ticket = 0;
    const QolResult result = qol::infra::commands_port().post(&ticket, &command);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeAutoSellRunNow(JNIEnv*, jclass) {
#if QOL_KERNEL_ONLY
    return JNI_FALSE;
#else
    // 保留 JNI 入口以兼容既有 external 签名；扫描已由 frame_task 周期任务（每 60 帧）负责，
    // 不再提供单次立即扫描：60 帧等待可接受。
    return JNI_TRUE;
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeAutoSellStatusJson(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return env->NewStringUTF("{\"ok\":false,\"error\":\"kernel-only\"}");
#else
    return env->NewStringUTF(autosell_status_json().c_str());
#endif
}
