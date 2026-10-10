// gamebridge_autosell.cpp —— 自动出售 JNI 薄层（阶段 B）。
//
// 仅做 JNI 参数/返回值转换；配置与状态逻辑在 plugins/autosell/*。

#include "gamebridge_internal.h"

#if !QOL_KERNEL_ONLY
#include "contracts/autosell.h"
#include "contracts/commands.h"
#include "plugins/autosell/config.h"
#include "plugins/autosell/store.h"
#include "infra/commands.h"

namespace {

// M-11：native 侧入参钳制，与 Kotlin 校验一致（不可信输入按边界收敛，不越界驱动规则）。
int clamp_range(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

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
    // 更新运行时配置并经 autosell slot-data participant 按当前 session 即时持久化。
    autosell_store_commit_config(cfg);
    return JNI_TRUE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellEnabled(JNIEnv*, jclass, jboolean enabled) {
    // Frozen global-config compatibility stub; writes belong to core.settings' native
    // single-key owner-token path. nativeSetAutoSellConfig remains per-save-slot rule data.
    (void)enabled;
    return JNI_FALSE;
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
