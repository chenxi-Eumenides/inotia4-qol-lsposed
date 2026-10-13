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
#include "host/config_platform_bridge.h"
#include "host/features/production.h"
#include "host/features/receipt_formatter.h"

#include <memory>
#include <mutex>
#include <limits>
#include <new>
#include <unordered_map>

namespace {
JavaVM* g_cached_jvm = nullptr;

struct RuntimeCallbackContext : std::enable_shared_from_this<RuntimeCallbackContext> {
    JavaVM* vm = nullptr;
    jclass callbacks = nullptr;
    jmethodID method = nullptr;
    uint64_t subscription_id = 0;

    ~RuntimeCallbackContext() {
        if (vm == nullptr || callbacks == nullptr) return;
        JNIEnv* env = nullptr;
        bool attached = false;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
            attached = true;
        }
        env->DeleteGlobalRef(callbacks);
        if (attached) vm->DetachCurrentThread();
    }
};

std::mutex g_runtime_callbacks_mutex;
std::unordered_map<uint64_t, std::shared_ptr<RuntimeCallbackContext>> g_runtime_callbacks;

constexpr std::size_t kMaxReceiptCallbackContexts = 256;

struct ConfigReceiptCallbackContext
    : std::enable_shared_from_this<ConfigReceiptCallbackContext> {
    JavaVM* vm = nullptr;
    jclass callbacks = nullptr;
    jmethodID method = nullptr;
    uint64_t request_id = 0;
    uint64_t jni_subscription_id = 0;
    uint64_t controller_subscription_id = 0;
    bool registration_in_progress = true;
    bool completed = false;

    ~ConfigReceiptCallbackContext() {
        if (vm == nullptr || callbacks == nullptr) return;
        JNIEnv* env = nullptr;
        bool attached = false;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                QOL_LOG_ERROR(QolDomain::kCore,
                              "config receipt callback could not attach to release class ref");
                return;
            }
            attached = true;
        }
        env->DeleteGlobalRef(callbacks);
        if (attached) vm->DetachCurrentThread();
    }
};

std::mutex g_config_receipt_callbacks_mutex;
std::unordered_map<uint64_t, std::shared_ptr<ConfigReceiptCallbackContext>>
    g_config_receipt_callbacks;
uint64_t g_next_config_receipt_subscription_id = 1;

uint64_t allocate_config_receipt_subscription_id_locked() {
    const uint64_t max_id = static_cast<uint64_t>(std::numeric_limits<jlong>::max());
    for (std::size_t attempt = 0; attempt <= kMaxReceiptCallbackContexts; ++attempt) {
        if (g_next_config_receipt_subscription_id == 0 ||
            g_next_config_receipt_subscription_id > max_id)
            g_next_config_receipt_subscription_id = 1;
        const uint64_t candidate = g_next_config_receipt_subscription_id++;
        if (g_config_receipt_callbacks.find(candidate) == g_config_receipt_callbacks.end())
            return candidate;
    }
    return 0;
}

void erase_config_receipt_context(
    const std::shared_ptr<ConfigReceiptCallbackContext>& context) {
    std::lock_guard<std::mutex> lock(g_config_receipt_callbacks_mutex);
    const auto found = g_config_receipt_callbacks.find(context->jni_subscription_id);
    if (found != g_config_receipt_callbacks.end() && found->second == context)
        g_config_receipt_callbacks.erase(found);
}

void finish_config_receipt_callback(
    const std::shared_ptr<ConfigReceiptCallbackContext>& context) {
    std::lock_guard<std::mutex> lock(g_config_receipt_callbacks_mutex);
    const auto found = g_config_receipt_callbacks.find(context->jni_subscription_id);
    if (found == g_config_receipt_callbacks.end() || found->second != context) return;
    context->completed = true;
    if (!context->registration_in_progress) g_config_receipt_callbacks.erase(found);
}

void clear_receipt_java_exception(JNIEnv* env, const char* operation) {
    if (env == nullptr || !env->ExceptionCheck()) return;
    QOL_LOG_ERROR(QolDomain::kCore, "config receipt callback JNI failure during %s", operation);
    env->ExceptionDescribe();
    env->ExceptionClear();
}

void deliver_config_receipt(void* raw_context,
                            const qol::contracts::FeatureReceipt& receipt) noexcept {
    if (raw_context == nullptr) return;
    auto* raw = static_cast<ConfigReceiptCallbackContext*>(raw_context);
    std::shared_ptr<ConfigReceiptCallbackContext> context;
    try {
        context = raw->shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        QOL_LOG_ERROR(QolDomain::kCore, "config receipt callback context expired");
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    if (context->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (context->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            QOL_LOG_ERROR(QolDomain::kCore, "config receipt callback thread attach failed");
            finish_config_receipt_callback(context);
            return;
        }
        attached = true;
    }

    std::string receipt_json;
    try {
        receipt_json = qol::host::feature_receipt_json(receipt);
    } catch (const std::bad_alloc&) {
        QOL_LOG_ERROR(QolDomain::kCore, "config receipt callback JSON allocation failed");
        if (attached) context->vm->DetachCurrentThread();
        finish_config_receipt_callback(context);
        return;
    }
    jstring json = env->NewStringUTF(receipt_json.c_str());
    if (json == nullptr) {
        clear_receipt_java_exception(env, "NewStringUTF");
    } else {
        env->CallStaticVoidMethod(context->callbacks, context->method,
                                  static_cast<jlong>(receipt.request_id), json);
        env->DeleteLocalRef(json);
        clear_receipt_java_exception(env, "onConfigReceipt");
    }
    if (attached) context->vm->DetachCurrentThread();
    finish_config_receipt_callback(context);
}

void deliver_runtime_effect(void* raw, const qol::contracts::QolPluginRuntimeEffect* effect) {
    if (raw == nullptr || effect == nullptr) return;
    auto* raw_context = static_cast<RuntimeCallbackContext*>(raw);
    std::shared_ptr<RuntimeCallbackContext> context;
    try { context = raw_context->shared_from_this(); }
    catch (const std::bad_weak_ptr&) { return; }
    JNIEnv* env = nullptr;
    bool attached = false;
    if (context->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (context->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    const char* plugin_id = effect->plugin_id != nullptr ? effect->plugin_id : "";
    const char* config_json = effect->config_json != nullptr ? effect->config_json : "{}";
    jstring plugin = env->NewStringUTF(plugin_id);
    jstring config = env->NewStringUTF(config_json);
    if (plugin != nullptr && config != nullptr) {
        env->CallStaticVoidMethod(context->callbacks, context->method, plugin,
                                  static_cast<jlong>(effect->event_revision),
                                  static_cast<jlong>(effect->config_revision),
                                  effect->actual_enabled ? JNI_TRUE : JNI_FALSE,
                                  effect->desired_enabled ? JNI_TRUE : JNI_FALSE, config);
    }
    if (plugin != nullptr) env->DeleteLocalRef(plugin);
    if (config != nullptr) env->DeleteLocalRef(config);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) context->vm->DetachCurrentThread();
}

bool java_string(JNIEnv* env, jstring value, std::string* out) {
    if (env == nullptr || value == nullptr || out == nullptr) return false;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return false;
    out->assign(chars);
    env->ReleaseStringUTFChars(value, chars);
    return true;
}
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
    // Path-only bootstrap records the config directory before this call; Store init/seed runs in
    // assembly after all descriptors are registered (production_feature_bootstrap). Gate on the
    // recorded request rather than completion, otherwise init and assembly would wait on each other.
    if (!qol::host::config_store_bootstrap_requested()) {
        return JNI_FALSE;
    }
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
        // 帧缓存预取按 Store 的 apiEnabled 在 assembly/install 完成后启停，避免早于 builder 与 frame source。
        // 设置页/备份面板/自动出售面板注入已迁移为插件：core.ui 的 own:popup.settings、
        // save_backup 的 own:popup.savebackup 与 autosell 的 own:popup.autosell 及入口
        // call patch 均由 assembly_init 经资源事务声明式安装（无 nativeInit 直调）。
        // 世界传送已迁移为 world_teleport 插件：入口 patch 由插件 install 负责（assembly_init 装配）；
        // 选择框组件为独立插件 native_choice（provides `ui_choice`），消费者经能力契约获取。
        // 合成器界面层已迁移为 craft_ui 插件（assembly 注册，声明式安装 12 条 UIMix hook）；
        // 结果层 custom_recipe 插件 provides `craft_result`，动态配方重建在插件 install 内注册。
        if (!qol::host::assembly_init()) {
            QOL_LOG_ERROR(QolDomain::kCore,
                          "nativeInit: assembly failed; native bootstrap remains unavailable");
            return JNI_FALSE;
        }
        // 特殊装备解锁已迁移为 special_equip 插件（assembly 注册，宿主声明式安装两个入口 hook）。
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Frozen compatibility setter. Global config writes must use core.settings' native single-key
// owner-token path; runtime changes are applied only after that Store commit.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetApiEnabled(JNIEnv*, jclass, jboolean enabled) {
    (void)enabled;
    return JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetBaseAddr(JNIEnv*, jclass) {
    return static_cast<jlong>(g_base);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRuntimeEffectSubscribe(JNIEnv* env, jclass,
                                                                jstring plugin_id,
                                                                jclass callbacks) {
    std::string id;
    if (!java_string(env, plugin_id, &id) || id.empty() || callbacks == nullptr) return 0;
    auto context = std::make_shared<RuntimeCallbackContext>();
    if (env->GetJavaVM(&context->vm) != JNI_OK) return 0;
    context->method = env->GetStaticMethodID(
        callbacks, "onRuntimeEffect", "(Ljava/lang/String;JJZZLjava/lang/String;)V");
    if (context->method == nullptr) return 0;
    context->callbacks = static_cast<jclass>(env->NewGlobalRef(callbacks));
    if (context->callbacks == nullptr) return 0;
    uint64_t subscription_id = 0;
    const QolResult result = qol::host::runtime_effects_subscribe_trusted(
        id.c_str(), &deliver_runtime_effect, context.get(), &subscription_id);
    if (!qol::contracts::succeeded(result)) return 0;
    context->subscription_id = subscription_id;
    {
        std::lock_guard<std::mutex> lock(g_runtime_callbacks_mutex);
        g_runtime_callbacks.emplace(subscription_id, std::move(context));
    }
    return static_cast<jlong>(subscription_id);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeRuntimeEffectUnsubscribe(JNIEnv*, jclass,
                                                                   jlong subscription_id) {
    if (subscription_id <= 0) return JNI_FALSE;
    std::shared_ptr<RuntimeCallbackContext> context;
    {
        std::lock_guard<std::mutex> lock(g_runtime_callbacks_mutex);
        const auto found = g_runtime_callbacks.find(static_cast<uint64_t>(subscription_id));
        if (found == g_runtime_callbacks.end()) return JNI_FALSE;
        context = found->second;
    }
    const QolResult result = qol::host::runtime_effects_unsubscribe_trusted(
        static_cast<uint64_t>(subscription_id));
    if (!qol::contracts::succeeded(result)) return JNI_FALSE;
    {
        std::lock_guard<std::mutex> lock(g_runtime_callbacks_mutex);
        g_runtime_callbacks.erase(static_cast<uint64_t>(subscription_id));
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativePluginConfigSubscribeReceipt(
    JNIEnv* env, jclass, jlong request_id, jclass callbacks) {
    if (env == nullptr || request_id <= 0 || callbacks == nullptr) return 0;
    std::shared_ptr<ConfigReceiptCallbackContext> context;
    try {
        context = std::make_shared<ConfigReceiptCallbackContext>();
    } catch (const std::bad_alloc&) {
        QOL_LOG_ERROR(QolDomain::kCore, "config receipt callback context allocation failed");
        return 0;
    }
    if (env->GetJavaVM(&context->vm) != JNI_OK) return 0;
    context->method = env->GetStaticMethodID(
        callbacks, "onConfigReceipt", "(JLjava/lang/String;)V");
    if (context->method == nullptr) {
        clear_receipt_java_exception(env, "GetStaticMethodID");
        return 0;
    }
    context->callbacks = static_cast<jclass>(env->NewGlobalRef(callbacks));
    if (context->callbacks == nullptr) {
        clear_receipt_java_exception(env, "NewGlobalRef");
        return 0;
    }
    context->request_id = static_cast<uint64_t>(request_id);

    try {
        std::lock_guard<std::mutex> lock(g_config_receipt_callbacks_mutex);
        if (g_config_receipt_callbacks.size() >= kMaxReceiptCallbackContexts) return 0;
        const uint64_t id = allocate_config_receipt_subscription_id_locked();
        if (id == 0) return 0;
        context->jni_subscription_id = id;
        g_config_receipt_callbacks.emplace(id, context);
    } catch (const std::bad_alloc&) {
        QOL_LOG_ERROR(QolDomain::kCore, "config receipt callback registration allocation failed");
        return 0;
    }

    uint64_t controller_subscription_id = 0;
    QolResult result{};
    try {
        result = qol::host::production_feature_controller().subscribe_receipt(
            context->request_id, &deliver_config_receipt, context.get(),
            &controller_subscription_id);
    } catch (const std::bad_alloc&) {
        QOL_LOG_ERROR(QolDomain::kCore, "config receipt observer registration allocation failed");
        erase_config_receipt_context(context);
        return 0;
    } catch (...) {
        QOL_LOG_ERROR(QolDomain::kCore, "config receipt observer registration failed");
        erase_config_receipt_context(context);
        return 0;
    }
    if (!qol::contracts::succeeded(result)) {
        erase_config_receipt_context(context);
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_config_receipt_callbacks_mutex);
        const auto found = g_config_receipt_callbacks.find(context->jni_subscription_id);
        if (found == g_config_receipt_callbacks.end() || found->second != context) return 0;
        context->controller_subscription_id = controller_subscription_id;
        context->registration_in_progress = false;
        if (context->completed || controller_subscription_id == 0)
            g_config_receipt_callbacks.erase(found);
    }
    return static_cast<jlong>(context->jni_subscription_id);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativePluginConfigUnsubscribeReceipt(
    JNIEnv*, jclass, jlong subscription_id) {
    if (subscription_id <= 0) return JNI_FALSE;
    std::shared_ptr<ConfigReceiptCallbackContext> context;
    uint64_t controller_subscription_id = 0;
    {
        std::lock_guard<std::mutex> lock(g_config_receipt_callbacks_mutex);
        const auto found = g_config_receipt_callbacks.find(
            static_cast<uint64_t>(subscription_id));
        if (found == g_config_receipt_callbacks.end()) return JNI_FALSE;
        context = found->second;
        controller_subscription_id = context->controller_subscription_id;
    }
    if (controller_subscription_id == 0) return JNI_FALSE;

    const QolResult result = qol::host::production_feature_controller().unsubscribe_receipt(
        controller_subscription_id);
    if (qol::contracts::succeeded(result) || result.code == kQolErrNotFound)
        erase_config_receipt_context(context);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}
