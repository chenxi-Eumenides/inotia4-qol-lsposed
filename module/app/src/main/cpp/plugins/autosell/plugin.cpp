#include "plugins/autosell/plugin.h"

#include <atomic>
#include <cstring>

#include "contracts/events.h"
#include "core/native/qol_log.h"
#include "plugins/autosell/config.h"
#include "plugins/autosell/scan.h"
#include "infra/config.h"
#include "infra/events.h"

// 自动出售插件：零资源常驻。门禁来源为配置键 `plugin.autosell.enabled`
// （nativeSetAutoSellEnabled → kCommandSetConfigBool）。零资源插件没有宿主资源 gate，
// 因此经 kEventConfigChanged 订阅驱动；install/on_enabled 时按配置初值应用一次。
//
// 过渡依赖（清除阶段）：
// - save_enter/save_exit legacy 设施 → B4（本插件的进档/退档回调随之迁移）。
// - sidecar 经 Kotlin ModuleSaveStore 桥 → B4 `infra/store`。
// - `game_access.h/game_state.h/game_symbols.h` 直取 → game/access 批次。
// - `inventory_trade` → `domains/inventory`。
//
// UI 门控入口 `autosell_ui_set_enabled` 定义在 feature/ui（D1 迁移）：此处前置声明，
// 避免 feature→feature include（special_equip 先例）；D1 后改为 UI 侧消费者接口。

void autosell_ui_set_enabled(bool enabled);

namespace qol::features::autosell {
namespace {

constexpr const char* kPluginId = "autosell";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kGateKey = "plugin.autosell.enabled";

infra::SubscriptionId g_config_subscription = 0;
std::atomic<bool> g_gate{false};

// 门禁应用：同步扫描任务「已武装」标志与 UI 入口按钮/面板门控（现行为不变）。
void apply_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    autosell_set_global_enabled(enabled);
    autosell_ui_set_enabled(enabled);
}

void on_config_changed(void* ctx, const void* payload) {
    (void)ctx;
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::ConfigChangedEvent*>(payload);
    if (event->key == nullptr || std::strcmp(event->key, kGateKey) != 0) return;
    apply_gate(event->int_value != 0);
}

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // save_enter/save_exit 回调设施是共享过渡依赖（B4 迁移）；注册对同一 fn+ctx 幂等，
    // activate 重入安全。
    autosell_register_save_enter();
    autosell_register_save_exit();
    // 门禁初值取配置；订阅配置变更（重复订阅会留孤儿订阅，已订阅则跳过）。
    apply_gate(infra::config_get_bool(kGateKey, false));
    if (g_config_subscription == 0) {
        g_config_subscription = infra::subscribe(contracts::kEventConfigChanged,
                                                 &on_config_changed, nullptr);
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    if (g_config_subscription != 0) {
        infra::unsubscribe(g_config_subscription);
        g_config_subscription = 0;
    }
    autosell_unregister_save_callbacks();
    apply_gate(false);
}

// 零资源、无资源 gate：启停只切业务门禁，不做资源操作（§3.3）。宿主对零资源插件
// 会在装配时调用 on_enabled，这里按配置初值应用，不无条件开启。
void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    apply_gate(infra::config_get_bool(kGateKey, false));
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    apply_gate(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_gate.load(std::memory_order_acquire);
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    0, nullptr,   // resources（零资源）
    0, nullptr,   // jni（JNI 入口由 bridge/native/gamebridge_autosell.cpp 保留）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（无资源）
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::autosell
