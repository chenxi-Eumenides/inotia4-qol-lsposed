#pragma once

#include <cstdint>
#include <string>

// 存档管理器游戏内面板（原 feature/ui/game_ui_savebackup，D1-b2 迁入 save_backup 插件）。
//
// 复用 IAP 屏蔽后的 F_PANEL_UNK2_ENTER 死条目（Scene_Init_POPUP_SC_INAPP_GOODS）承载三栏
// （存档槽 / 操作 / 备份列表）弹出面板；PopupState 占用为 `own:popup.savebackup`
// （kOwnership/kFailSoft，plugin.cpp 申报），acquire/release 走 hooks/popup_state。
//
// 面板经 ui_panels 能力被打开（设置页「存档备份」动作项 → core.ui 面板栈 →
// 本插件 provider.open）；嵌套 push 的栈顶校验留在本 provider 内，失败由 core.ui 面板栈
// 「关栈顶再开」兜底。

namespace qol::features::save_backup {

// ---- JNI / 调试端点（原 data_savebackup_ui_*，名与签名保持）----
std::string data_savebackup_ui_inject();
std::string data_savebackup_ui_status_json();
std::string data_savebackup_ui_restore();
std::string data_savebackup_ui_open_panel();
std::string data_savebackup_ui_self_check();

// 测试用：注入 map_name（绕过 JSON 解析），供 host 单测复用解析路径。
void save_backup_inject_map_names_for_test(const char* json);

// ---- PanelProviderSpec 实现（panel_id="save_backup"）----
bool savebackup_provider_open();
void savebackup_provider_close();
bool savebackup_provider_active();
const char* savebackup_provider_block_reason();

// ---- own:popup.savebackup（kOwnership）----
int32_t savebackup_popup_acquire(const char* id);
void savebackup_popup_release(const char* id);

}  // namespace qol::features::save_backup
