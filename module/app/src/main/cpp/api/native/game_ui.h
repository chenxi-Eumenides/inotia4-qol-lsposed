#pragma once

#include <cstdint>
#include <string>

// UI 域（parse 域）：调试 UI + 面板栈顶 + UI 状态判定。

std::string data_debug_ui_json();
uintptr_t data_popup_top_vma();
const char* data_top_panel_name();
const char* data_ui_screen();

// UI 操作：主菜单/面板开关。
std::string data_op_main_menu();
// owner 命令路径（ADR 0001 第 4 批）：同守卫与清理，状态机切换走逻辑帧内联（不跨帧同步等待）。
std::string data_op_main_menu_direct();
std::string data_op_panel_close();
std::string data_op_panel_open(const std::string& panel);
