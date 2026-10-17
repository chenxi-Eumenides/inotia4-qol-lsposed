#pragma once

#include <cstdint>
#include <string>

#include "../../../contracts/host_sdk.h"
#include "../../../contracts/game_host.h"

// core.ui 设置面板（原 feature/ui/game_ui_settings，D1-b2 迁移）。
//
// 面板本体：主菜单「更多游戏」按钮 ExecuteProc → ui_panels.open("settings") → 本 provider
// 经游戏 PopupState 死条目 F_PANEL_UNK1_ENTER（INAP_GEMSHOP）打开；配置页网格 + 分页 +
// 「存档备份」动作项（ui_panels.open("save_backup")）。
//
// PopupState 由 core.custom_ui 面板宿主唯一持有；本插件只提供页面回调。
//
// 「更多游戏」按钮是运行时控件实例的 ExecuteProc 槽（非稳定 GOT 槽），D1 暂以
// PtrHook + infra/scheduler 主线程帧任务绑定/释放；登记迁移豁免，P5/U11 由动态 UI 绑定器收口。

namespace qol::features::core_ui {

void settings_bind_sdk(const contracts::QolHostSdk* sdk, contracts::QolOwnerToken owner_token);
void settings_bind_game(const contracts::QolGameHost* game);

// 打开面板的本地薄入口：经必需能力 `ui_panels` 调用（ADR 0001 D2：能力 ID/版本不变，
// 只迁提供者），设置页因此不直接依赖框架实现。实现见 feature.cpp。
bool settings_panels_open(const char* panel_id);
void settings_panels_close(const char* panel_id);

// ---- JNI / 调试端点（原 data_settings_ui_*，名与签名保持）----
std::string data_settings_ui_inject();
std::string data_settings_ui_status_json();
std::string data_settings_ui_restore();
std::string data_settings_ui_open_panel();
std::string data_settings_ui_open_option();
void settings_register_config_bridge(void* env, void* bridge_class);

// 关闭设置面板（仅清本模块 panel 状态）：provider.close 复用；调用方负责游戏 pop 关闭。
void settings_ui_close_panel();

// ---- PanelProviderSpec 实现（panel_id="settings"）----
bool settings_provider_open();
void settings_provider_close();
bool settings_provider_active();
const char* settings_provider_block_reason();

const contracts::PanelCallbacks* settings_panel_callbacks();

// ---- 「更多游戏」按钮绑定（帧任务驱动）----
void settings_more_games_tick();            // 主线程帧任务调用
void settings_release_more_games();         // 停用 / 反注册时释放绑定
const char* settings_more_games_bind_state(); // waiting / live / stale / error

// GRPX_DrawPart ABI at the MainMenu More Games draw callsite (final argument is native unknown).
void settings_main_menu_more_games_draw(void* group, int32_t x, int32_t y, void* loc,
                                        int32_t type, int32_t flip, int32_t unknown);

}  // namespace qol::features::core_ui
