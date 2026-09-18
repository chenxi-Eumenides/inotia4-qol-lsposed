#pragma once

#include <cstdint>
#include <jni.h>
#include <string>

// core.ui 设置面板（原 feature/ui/game_ui_settings，D1-b2 迁移）。
//
// 面板本体：主菜单「更多游戏」按钮 ExecuteProc → ui_panels.open("settings") → 本 provider
// 经游戏 PopupState 死条目 F_PANEL_UNK1_ENTER（INAP_GEMSHOP）打开；配置页网格 + 分页 +
// 「存档备份」动作项（ui_panels.open("save_backup")）。
//
// PopupState 占用为 `own:popup.settings`（kOwnership，plugin.cpp 申报）：本文件只提供
// acquire/release 的原语调用与面板回调；线程纪律由资源事务（主线程）保证。
//
// 「更多游戏」按钮是运行时控件实例的 ExecuteProc 槽（非稳定 GOT 槽），D1 暂以
// PtrHook + infra/scheduler 主线程帧任务绑定/释放；登记迁移豁免，P5/U11 由动态 UI 绑定器收口。

namespace qol::features::core_ui {

// ---- JNI / 调试端点（原 data_settings_ui_*，名与签名保持）----
std::string data_settings_ui_inject();
std::string data_settings_ui_status_json();
std::string data_settings_ui_restore();
std::string data_settings_ui_open_panel();
std::string data_settings_ui_open_option();
void settings_register_config_bridge(JNIEnv* env, jclass bridge_class);

// 关闭设置面板（仅清本模块 panel 状态）：provider.close 复用；调用方负责游戏 pop 关闭。
void settings_ui_close_panel();

// ---- PanelProviderSpec 实现（panel_id="settings"）----
bool settings_provider_open();
void settings_provider_close();
bool settings_provider_active();
const char* settings_provider_block_reason();

// ---- own:popup.settings（kOwnership）----
int32_t settings_popup_acquire(const char* id);
void settings_popup_release(const char* id);

// ---- 「更多游戏」按钮绑定（帧任务驱动）----
void settings_more_games_tick();            // 主线程帧任务调用
void settings_release_more_games();         // 停用 / 反注册时释放绑定
const char* settings_more_games_bind_state(); // waiting / live / stale / error

}  // namespace qol::features::core_ui
