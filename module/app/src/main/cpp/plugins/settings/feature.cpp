#include "feature.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../../contracts/panels.h"
#include "../../contracts/ui.h"
#include "panels/settings.h"

namespace qol::features::core_ui {
namespace {

std::atomic<bool> g_enabled{false};
const contracts::QolHostSdk* g_sdk = nullptr;
uint64_t g_frame_task = 0;
const contracts::UiPanelsApi* g_panels_api = nullptr;

// 主线程帧任务：替代原后台轮询线程，推进「更多游戏」按钮绑定/重绑定。
int32_t frame_tick(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    if (g_enabled.load(std::memory_order_acquire)) {
        settings_more_games_tick();
    }
    return 1;  // 常驻：主菜单多次重建需重新绑定
}

// 设置页容器 provider（panel_id="settings"）。
const contracts::PanelProviderSpec kSettingsProvider = {
    "settings",
    "settings",
    &settings_provider_open,
    &settings_provider_close,
    &settings_provider_active,
    &settings_provider_block_reason,
    settings_panel_callbacks(),
};

}  // namespace

void panels_bind_sdk(const contracts::QolHostSdk* sdk, contracts::QolOwnerToken owner_token) {
    g_sdk = sdk;
    settings_bind_sdk(sdk, owner_token);
    // 依赖一律经能力取得（ADR 0001 D2）：能力 ID 与版本保持原样，改的只是提供者。
    g_panels_api = nullptr;
    if (sdk != nullptr && sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        if (contracts::succeeded(sdk->capabilities.require(
                contracts::kUiPanelsCapabilityId,
                {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}, &iface))) {
            g_panels_api = static_cast<const contracts::UiPanelsApi*>(iface);
        }
    }
}

bool settings_panels_open(const char* panel_id) {
    if (g_panels_api == nullptr || g_panels_api->open == nullptr) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            static const char kMsg[] = "panels_open skipped: ui_panels capability unavailable";
            (void)g_sdk->log.write(1, 12, "core.settings/feature.cpp", kMsg,
                                  static_cast<uint32_t>(sizeof(kMsg) - 1));
        }
        return false;
    }
    return g_panels_api->open(panel_id);
}

void settings_panels_close(const char* panel_id) {
    if (g_panels_api != nullptr && g_panels_api->close != nullptr) {
        g_panels_api->close(panel_id);
    }
}

bool plugin_enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void plugin_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        // 停用即释放运行时控件槽绑定（对象可能已销毁，释放自带可访问性守卫）。
        settings_release_more_games();
    }
}

bool panels_register() {
    if (g_sdk == nullptr || g_sdk->panels.register_provider == nullptr ||
        contracts::feature_control_if_available(g_sdk) == nullptr) return false;
    if (!contracts::succeeded(g_sdk->panels.register_provider(&kSettingsProvider))) return false;
    if (g_frame_task == 0 && g_sdk->scheduler.add != nullptr) {
        // 每帧（interval=1）：游戏 STATE_EnterMainMenu 会重填「更多游戏」ExecuteProc 槽为
        // 原版 GotoShowMoreGames，重绑间隔大于 1 帧时存在点击落回原版 gemshop 的窗口；
        // 每帧校验把窗口收窄到 1 帧。开销仅为每帧一次 screen 名比较 + 控件槽安全读 +
        // 指针比较，校验命中（常态）时无写操作，绑定/释放只在槽被改写或进出主菜单时发生。
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(1, &frame_tick, nullptr, 1, 0, &task))) {
            g_frame_task = task;
        }
    }
    return true;
}

void panels_unregister() {
    if (g_frame_task != 0) {
        if (g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
            (void)g_sdk->scheduler.remove(g_frame_task);
        }
        g_frame_task = 0;
    }
    settings_ui_close_panel();
    settings_release_more_games();
    g_enabled.store(false, std::memory_order_release);
    if (g_sdk != nullptr && g_sdk->panels.unregister_owner != nullptr) {
        (void)g_sdk->panels.unregister_owner("settings");
    }
    g_panels_api = nullptr;
    settings_bind_sdk(nullptr, contracts::kQolOwnerTokenInvalid);
    g_sdk = nullptr;
}

}  // namespace qol::features::core_ui
