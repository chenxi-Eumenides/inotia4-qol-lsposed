#pragma once

#include <cstdint>
#include <string>

// 存档管理器游戏内面板（ui-save-backup v0.7.x）：
// 复用 IAP 屏蔽后的 F_PANEL_UNK2_ENTER 死条目（Scene_Init_POPUP_SC_INAPP_GOODS），
// 在主菜单下注入一个三栏（存档槽 / 操作 / 备份列表）的弹出面板，提供
// 导出选中槽 / 导入选中备份到选中槽 / 删除选中备份 + 二次确认 + 结果提示。
//
// 设计要点（与 game_ui_settings.cpp 同构，独立文件避免混入设置页逻辑）：
// - 控件原语全部走 feature/ui/game_ui_kit.h，无列表/滚动控件，自造带翻页的虚拟列表。
// - root 不入控件树，触摸事件在 PopupState event 内自分发；root 的 CO_CONTROL_PROC
//   必须清零（否则 GetData=null SIGSEGV）。
// - 注入生命周期：仅在 main_menu 注入 state 条目；离开主菜单还原原始回调。
// - 嵌套 push 模式：设置页 keep open，本面板在它之上；如实测 push 行为异常退化为切换式。
// - API 守卫：data_op_enter_slot / data_op_create_slot 在本面板或设置页打开时返回
//   `{"ok":false,"error":"ui occupied: backup panel" / "ui occupied: settings panel"}`。

std::string data_savebackup_ui_inject();      // 启用懒注入（与 settings 等价）
std::string data_savebackup_ui_status_json(); // 状态 JSON
std::string data_savebackup_ui_restore();     // 还原 state 条目
std::string data_savebackup_ui_open_panel();  // 由设置页 push 调用

// 给 data_op_enter_slot/create_slot 的统一 UI 守卫；空串表示未占用，否则返回错误原因。
// 检查项：备份面板、设置面板是否激活（两套独立状态，分别独立判定）。
const char* savebackup_ui_block_reason();

// 测试用：备份面板自检（native 读目录返回 listJson）；Kotlin 端在初始化时同样会调用一次。
std::string data_savebackup_ui_self_check();

// 测试用：注入 map_name（绕过 set_map_names JSON 解析），供 host 单测复用解析路径。
void save_backup_inject_map_names_for_test(const char* json);
