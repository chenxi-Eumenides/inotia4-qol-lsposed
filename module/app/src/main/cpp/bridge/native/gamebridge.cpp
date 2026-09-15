#include "gamebridge_internal.h"
#include "core/native/frame_host.h"
#include "core/native/save_enter.h"
#include "core/native/save_exit.h"
#include "core/native/transition_dispatch.h"
#include "feature/autosell/autosell_config.h"
#include "feature/patch/native_inventory_hook.h"
#include "feature/attribute_range/game_ui_attr_range.h"
#include "feature/autosell/autosell_scan.h"
#include "feature/autosell/autosell_store.h"
#include "feature/save_backup/save_backup.h"
#include "feature/ui/game_ui_autosell.h"
#include "feature/ui/game_ui_charinfo_zh.h"
#include "feature/ui/module_text.h"
#include "feature/quest/quest_resync.h"
#include "feature/simple_mode/simple_mode.h"
#include "feature/world_teleport/world_teleport.h"

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
        // 模块自定义文本层：独占 MEMORYTEXT_GetText 热点 hook（全库唯一 owner），必须在
        // 任何「标记文本作用域」的功能之前安装 —— 否则消费者标了窗口也没有替换者。
        module_text::module_text_install_if_ready();
        charinfo_zh_install_if_ready();  // 角色面板：把绘制窗口标记为 kCharacterPanel 作用域
        save_backup_slot_delete_hook_install_if_ready();
        // 统一帧派发宿主（渲染开始前锚点）。自动出售扫描任务：全局开关武装 +
        // 进入存档后由 autosell_register_save_enter 的回调注册（见 autosell_scan.cpp）。
        if (frame_host_install_if_ready()) {
            autosell_set_host_installed(true);
        }
        // 角色升级所需经验游戏线程缓存：注册常驻逻辑帧任务（CHAR_GetNextExperience 非纯读，
        // 不能在预取/HTTP 线程调用）。帧宿主未安装时任务注册成功但不派发（惰性无效）。
        char_next_exp_cache_start();
        transition_dispatch_init();           // 状态转换：逻辑帧点消费（go_main_menu/enter_slot/create_slot）
        save_enter_init();                    // 进入存档回调：帧检测任务（world 就绪触发一次）
        save_enter_host_install_if_ready();   // 进入存档回调：读档/新档发起点 call_patch
        save_exit_host_install_if_ready();    // 退出存档回调：world->主菜单发起点 call_patch
        autosell_register_save_enter();       // 自动出售：进档加载 sidecar 配置并注册扫描任务
        autosell_register_save_exit();        // 自动出售：退档删除扫描任务
        quest_resync_register_save_enter();   // 任务完成度进档重算：world 就绪后一次性补 state=2
        // 帧缓存预取不再无条件启动：由 Kotlin 侧按 apiEnabled 调 nativeSetApiEnabled 决定
        settings_ui_start_auto_inject();
        autosell_ui_install_if_ready();  // 自动出售 UI：背包页入口按钮绘制宿主
        world_teleport_install_if_ready();  // 世界传送：地图名入口 patch + choice ExecuteProc hook
        custom_recipe_ui_install_if_ready();  // 自定义合成配方：UIMix 放料/合成/产物三处函数级 hook
        simple_mode_install_if_ready();  // 简单模式：CHAR_AddDamage 伤害倍率 + CHAR_UpdateAttrFromMonster 最大生命减半
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
