#include "gamebridge_internal.h"
#include "core/native/frame_host.h"
#include "feature/autosell/autosell_config.h"
#include "feature/patch/native_inventory_hook.h"
#include "feature/attribute_range/game_ui_attr_range.h"
#include "feature/autosell/autosell_store.h"
#include "feature/save_backup/save_backup.h"

namespace {
JavaVM* g_cached_jvm = nullptr;
}  // namespace

JavaVM* g_jvm() { return g_cached_jvm; }

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_cached_jvm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRegisterConfigBridge(JNIEnv* env, jclass, jclass bridge_class) {
    settings_register_config_bridge(env, bridge_class);
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRegisterExtensionBagUiBridge(JNIEnv* env, jclass, jclass bridge_class) {
    virtual_bag_ui_register_bridge(env, bridge_class);
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRegisterAutoSellStoreBridge(JNIEnv* env, jclass, jclass bridge_class) {
    autosell_store_register_bridge(env, bridge_class);
}

// JNI 导出层：仅做参数传递与字符串转换，逻辑在 game_access / game_data。

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeInit(JNIEnv*, jclass) {
    bool ok = bridge_init();
    if (ok) {
        inventory_native_hook_install_if_ready();
        attr_range_ui_install_if_ready();
        save_backup_slot_delete_hook_install_if_ready();
        // 统一帧派发宿主（渲染开始前锚点）。自动出售任务改为开关驱动：由
        // nativeSetAutoSellConfig -> autosell_apply_config 按 enabled 注册 / 删除。
        if (frame_host_install_if_ready()) {
            autosell_set_host_installed(true);
        }
        frame_cache_start();   // v0.4.59：存在 interval>0 槽时启动预取线程（自 game_access 移入）
        settings_ui_start_auto_inject();
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetBaseAddr(JNIEnv*, jclass) {
    return static_cast<jlong>(g_base);
}
