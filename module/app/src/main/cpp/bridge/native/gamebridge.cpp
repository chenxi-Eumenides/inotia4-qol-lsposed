#include "gamebridge_internal.h"
#include "core/native/frame_host.h"
#include "core/native/save_enter.h"
#include "core/native/save_exit.h"
#include "core/native/transition_dispatch.h"
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
        transition_dispatch_init();           // 状态转换：逻辑帧点消费（go_main_menu/enter_slot/create_slot）
        save_enter_init();                    // 进入存档回调：帧检测任务（world 就绪触发一次）
        save_enter_host_install_if_ready();   // 进入存档回调：读档/新档发起点 call_patch
        save_exit_host_install_if_ready();    // 退出存档回调：world->主菜单发起点 call_patch
        // 帧缓存预取不再无条件启动：由 Kotlin 侧按 apiEnabled 调 nativeSetApiEnabled 决定
        settings_ui_start_auto_inject();
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

// API 全局开关：启用时启动帧缓存预取线程，关闭时停止线程并 join（可再次启用）。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetApiEnabled(JNIEnv*, jclass, jboolean enabled) {
    if (enabled == JNI_TRUE) {
        frame_cache_start();
    } else {
        frame_cache_stop();
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetBaseAddr(JNIEnv*, jclass) {
    return static_cast<jlong>(g_base);
}
