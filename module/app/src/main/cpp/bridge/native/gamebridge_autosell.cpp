// gamebridge_autosell.cpp —— 自动出售 JNI 薄层（阶段 B）。
//
// 仅做 JNI 参数/返回值转换；配置与状态逻辑在 feature/autosell/autosell_config.*。

#include "gamebridge_internal.h"

#include "feature/autosell/autosell_config.h"
#include "feature/autosell/autosell_store.h"

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
    JNIEnv*, jclass, jboolean enabled, jboolean rarityEnabled, jint rarityThreshold,
    jboolean enhanceEnabled, jint enhanceThreshold, jboolean socketEnabled, jint socketThreshold,
    jboolean gemTierEnabled, jint gemTierThreshold, jint specialMask) {
    autosell::Config cfg;
    cfg.enabled = enabled == JNI_TRUE;
    cfg.rarity_enabled = rarityEnabled == JNI_TRUE;
    cfg.rarity_threshold = clamp_range(static_cast<int>(rarityThreshold), 0, 4);
    cfg.enhance_enabled = enhanceEnabled == JNI_TRUE;
    cfg.enhance_threshold = static_cast<int>(enhanceThreshold) < 0
        ? 0 : static_cast<int>(enhanceThreshold);
    cfg.socket_enabled = socketEnabled == JNI_TRUE;
    cfg.socket_threshold = clamp_range(static_cast<int>(socketThreshold), 0, 15);
    cfg.gem_tier_enabled = gemTierEnabled == JNI_TRUE;
    cfg.gem_tier_threshold = clamp_range(static_cast<int>(gemTierThreshold), 0, 4);
    cfg.special_mask = static_cast<uint32_t>(specialMask);
    cfg.special_enabled = cfg.special_mask != 0u;
    autosell_set_runtime_config(cfg);
    // 按存档持久化：仅当前存档槽合法时直写 sidecar `autosell` section（不进 journal）。
    const int slot = current_save_slot();
    if (valid_persist_slot(slot)) {
        autosell_store_persist(slot, cfg);
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeAutoSellRunNow(JNIEnv*, jclass) {
    autosell_request_immediate_run();
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeAutoSellStatusJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(autosell_status_json().c_str());
}
