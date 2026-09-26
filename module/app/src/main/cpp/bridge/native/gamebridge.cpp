#include "gamebridge_internal.h"
#include "game/clock.h"
#include "infra/log/qol_log.h"
#include "patch/save_enter.h"
#include "patch/save_exit.h"
#include "infra/frame/transition_dispatch.h"
#if !QOL_KERNEL_ONLY
#include "plugins/autosell/config.h"
#endif
#include "host/assembly.h"
#include "host/core_patches.h"
#include "host/frame_source.h"

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
#if !QOL_KERNEL_ONLY
    qol::features::core_ui::settings_register_config_bridge(env, bridge_class);
#else
    (void)env;
    (void)bridge_class;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRegisterExtensionBagUiBridge(JNIEnv* env, jclass, jclass bridge_class) {
#if !QOL_KERNEL_ONLY
    virtual_bag_ui_register_bridge(env, bridge_class);
#else
    (void)env;
    (void)bridge_class;
#endif
}

// JNI 导出层：仅做参数传递与字符串转换，逻辑在 game/access/game_access / game/state/game_state。

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeInit(JNIEnv*, jclass) {
    // T2n-2：日志 sink/帧 provider 初始化搬至宿主装配（原 game/access 的 bridge_init 内，
    // 时序逐字保持：先初始化，再 bridge_init 内变体识别等日志）。帧号经 game/clock 同源。
    qol_log_set_frame_provider(&qol::game::frame_count);
    qol_log_init();
    // 启动期常驻 patch 入口注入：必须在 bridge_init() 之前（bridge_init 内调用）。
    bridge_set_boot_patch_hook(&qol::host::core_patches_install);
    bool ok = bridge_init();
    if (ok) {
        // T2h-B'：向 infra game_cache 注入 legacy JSON builder（K1），必须在任何
        // data_*_json 读取/frame_cache_start 之前，行为/线程与现状逐字一致。
        qol::host::install_legacy_cache_builders();
        // T2h-B1（v3 A-simple）：player/party/skills 三槽覆盖为 owner 适配器（失败 fallback
        // legacy）；kernel 无 core.character → require 失败 → 仍走 legacy，行为不变。
        qol::host::install_owner_cache_builders();
        // 库存行为 hook 已迁移为 inventory_hooks 插件（assembly 注册，宿主声明式安装 24 条入口）。
        // 属性范围已拆为 attribute_quality provider 与 attribute_range_display 消费者。
        // 模块自定义文本层已迁移为 text_layer 插件（共享资源 + 能力 + 角色面板作用域标记，
        // assembly 注册，宿主声明式安装）。
        // 删档同步 PtrHook 已随 save_backup 插件迁移：由其 install 安装（assembly_init）。
        // 统一帧派发宿主（渲染开始前锚点）。自动出售扫描任务：全局开关武装 +
        // 进入存档后由 autosell 插件注册的 save-enter 回调按存档配置同步（见 plugin.cpp）。
        if (qol::host::frame_source_install()) {
#if !QOL_KERNEL_ONLY
            autosell_set_host_installed(true);
#endif
        }
        // 角色升级所需经验游戏线程缓存：注册常驻逻辑帧任务（CHAR_GetNextExperience 非纯读，
        // 不能在预取/HTTP 线程调用）。帧宿主未安装时任务注册成功但不派发（惰性无效）。
        char_next_exp_cache_start();
        transition_dispatch_init();           // 状态转换：逻辑帧点消费（go_main_menu/enter_slot/create_slot）
        save_enter_init();                    // 进入存档回调：帧检测任务（world 就绪触发一次）
        save_enter_host_install_if_ready();   // 进入存档回调：读档/新档发起点（指令改写）
        save_exit_host_install_if_ready();    // 退出存档回调：world->主菜单发起点（指令改写）
        // 自动出售已迁移为 autosell 插件（assembly 注册）：save_enter/save_exit 回调与
        // 扫描任务生命周期由插件 install/on_enabled 管理。
        // 任务完成度进档重算已迁移为 quest 插件（plugin.cpp install 注册 save_enter 回调），
        // 由 assembly_init 统一装配。
        // 帧缓存预取不再无条件启动：由 Kotlin 侧按 apiEnabled 调 nativeSetApiEnabled 决定
        // 设置页/备份面板/自动出售面板注入已迁移为插件：core.ui 的 own:popup.settings、
        // save_backup 的 own:popup.savebackup 与 autosell 的 own:popup.autosell 及入口
        // call patch 均由 assembly_init 经资源事务声明式安装（无 nativeInit 直调）。
        // 世界传送已迁移为 world_teleport 插件：入口 patch 由插件 install 负责（assembly_init 装配）；
        // 选择框组件为独立插件 native_choice（provides `ui_choice`），消费者经能力契约获取。
        // 合成器界面层已迁移为 craft_ui 插件（assembly 注册，声明式安装 12 条 UIMix hook）；
        // 结果层 custom_recipe 插件 provides `craft_result`，动态配方重建在插件 install 内注册。
        qol::host::assembly_init();  // 插件装配：注册内置静态插件 + 声明式安装资源 + 命令帧排空
        // 特殊装备解锁已迁移为 special_equip 插件（assembly 注册，宿主声明式安装两个入口 hook）。
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
