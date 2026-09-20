#include "feature.h"

#include <atomic>
#include <cstdint>

#include "../../contracts/panels.h"
#include "panels/settings.h"
#include "panels/stack.h"

namespace qol::features::core_ui {
namespace {

std::atomic<bool> g_enabled{false};
const contracts::QolHostSdk* g_sdk = nullptr;
uint64_t g_frame_task = 0;

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
    "core.ui",
    &settings_provider_open,
    &settings_provider_close,
    &settings_provider_active,
    &settings_provider_block_reason,
};

// 设置页「存档备份」动作项：owner=core.ui（页面归属），目标 panel 归 save_backup 插件。
// 当前设置页仍用静态 11 项表渲染；本项为面板注册表的前向兼容登记（数据化留 D1-b4）。
const contracts::PanelItemSpec kSaveBackupItem = {
    "core.ui.item.save_backup",
    "core.ui",
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
    ui_panels_bind_sdk(sdk);
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
    g_enabled.store(false, std::memory_order_release);
    if (g_sdk != nullptr && g_sdk->panels.unregister_owner != nullptr) {
        (void)g_sdk->panels.unregister_owner("core.ui");
    }
}

}  // namespace qol::features::core_ui
