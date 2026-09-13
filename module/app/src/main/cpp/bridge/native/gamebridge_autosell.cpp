// gamebridge_autosell.cpp —— 自动出售 JNI 薄层（阶段 B）。
//
// 仅做 JNI 参数/返回值转换；配置与状态逻辑在 feature/autosell/autosell_config.*。

#include "gamebridge_internal.h"

#include "feature/autosell/autosell_config.h"
#include "feature/autosell/autosell_scan.h"
#include "feature/autosell/autosell_store.h"
#include "feature/ui/game_ui_autosell.h"

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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellConfig(
    JNIEnv*, jclass, jboolean enabled, jint rarity, jint enhance, jint socket, jint gemTier,
    jint specialMask, jint gemRange) {
    autosell::Config cfg;
    // 值即开关：0=关闭，正整数为 1-based 档位；按边界钳制不可信输入。
    cfg.enabled = enabled == JNI_TRUE;
    cfg.rarity = clamp_range(static_cast<int>(rarity), 0, 5);
    cfg.enhance = clamp_range(static_cast<int>(enhance), 0, 32);
    cfg.socket = clamp_range(static_cast<int>(socket), 0, 16);
    cfg.gem_tier = clamp_range(static_cast<int>(gemTier), 0, 5);
    cfg.gem_range = clamp_range(static_cast<int>(gemRange), 0, 5);
    const int mask = static_cast<int>(specialMask);
    cfg.special_mask = static_cast<uint32_t>(mask < 0 ? 0 : mask);
    // 写入运行时配置并按 enabled 注册 / 删除 60 帧周期扫描任务。
    autosell_apply_config(cfg);
    // 按存档持久化：仅当前存档槽合法时直写 sidecar `autosell` section（不进 journal）。
    const int slot = current_save_slot();
    if (valid_persist_slot(slot)) {
        autosell_store_persist(slot, cfg);
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellEnabled(JNIEnv*, jclass, jboolean enabled) {
    // 全局开关同时驱动：扫描任务注册/删除 + 背包页入口按钮/面板门控。
    autosell_set_global_enabled(enabled == JNI_TRUE);
    autosell_ui_set_enabled(enabled == JNI_TRUE);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeAutoSellRunNow(JNIEnv*, jclass) {
    // 保留 JNI 入口以兼容既有 external 签名；扫描已由 frame_task 周期任务（每 60 帧）负责，
    // 不再提供单次立即扫描（2026-09-13 裁决：60 帧等待可接受）。
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeAutoSellStatusJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(autosell_status_json().c_str());
}
