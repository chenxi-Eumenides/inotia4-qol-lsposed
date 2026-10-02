#include "feature.h"

#include <atomic>
#include <cstdint>

#include "../../contracts/panels.h"
#include "../../contracts/ui.h"
#include "panels/settings.h"
#include "panels/settings_ui_local.h"

namespace qol::features::core_ui {
namespace {

std::atomic<bool> g_enabled{false};
const contracts::QolHostSdk* g_sdk = nullptr;
uint64_t g_frame_task = 0;
const contracts::UiPanelsApi* g_panels_api = nullptr;
const contracts::QolUiComponents* g_components_api = nullptr;

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
    "core.settings",
    &settings_provider_open,
    &settings_provider_close,
    &settings_provider_active,
    &settings_provider_block_reason,
    settings_panel_callbacks(),
};

// 设置页「存档备份」动作项：owner=core.ui（页面归属），目标 panel 归 save_backup 插件。
// 当前设置页仍用静态 11 项表渲染；本项为面板注册表的前向兼容登记（数据化留 D1-b4）。
const contracts::PanelItemSpec kSaveBackupItem = {
    "core.ui.item.save_backup",
    "core.settings",
    "存档备份",
    contracts::PanelItemKind::kAction,
    nullptr,        // config_key（动作项）
    "save_backup",  // panel_id
    nullptr,        // page_hint
};

}  // namespace

void panels_bind_sdk(const contracts::QolHostSdk* sdk) {
    g_sdk = sdk;
    settings_bind_sdk(sdk);
    // 依赖一律经能力取得（ADR 0001 D2）：能力 ID 与版本保持原样，改的只是提供者。
    g_panels_api = nullptr;
    g_components_api = nullptr;
    if (sdk != nullptr && sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        if (contracts::succeeded(sdk->capabilities.require(
                contracts::kUiPanelsCapabilityId,
                {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}, &iface))) {
            g_panels_api = static_cast<const contracts::UiPanelsApi*>(iface);
        }
        iface = nullptr;
        if (contracts::succeeded(sdk->capabilities.require(
                contracts::kUiComponentsCapabilityId,
                {contracts::kUiContractMajor, contracts::kUiContractMinor}, &iface))) {
            g_components_api = static_cast<const contracts::QolUiComponents*>(iface);
        }
    }
    // 设置页自持 helper 的依赖注入：宿主 UI 适配表 + 组件能力（不再引用框架实现）。
    settings_ui_bind(sdk != nullptr ? sdk->ui : nullptr, g_components_api);
}

bool settings_panels_open(const char* panel_id) {
    if (g_panels_api == nullptr || g_panels_api->open == nullptr) return false;
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
        g_sdk->panels.register_item == nullptr) return false;
    if (!contracts::succeeded(g_sdk->panels.register_provider(&kSettingsProvider))) return false;
    if (!contracts::succeeded(g_sdk->panels.register_item(&kSaveBackupItem))) return false;
    if (g_frame_task == 0 && g_sdk->scheduler.add != nullptr) {
        // 15 帧一次（≈250ms，接近原轮询线程 300ms 粒度）：绑定动作轻量，无需逐帧。
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(1, &frame_tick, nullptr, 15, 0, &task))) {
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
    settings_release_more_games();
    settings_ui_unbind();
    g_enabled.store(false, std::memory_order_release);
    if (g_sdk != nullptr && g_sdk->panels.unregister_owner != nullptr) {
        (void)g_sdk->panels.unregister_owner("core.settings");
    }
}

}  // namespace qol::features::core_ui
