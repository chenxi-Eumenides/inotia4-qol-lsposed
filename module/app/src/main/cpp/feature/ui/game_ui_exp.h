#pragma once

#include <cstdint>
#include <string>

// UI 自定义实验域（ui-exp v0.6.7）：验证 docs/system/ui.md §6 的 5 种自定义 UI 方式。
// 实验①改按钮行为 / ②往面板加控件 / ③自定义对话框 / ④改文本外观 / ⑤全新面板（state list 注入）。

std::string data_exp1_btn_behavior();
std::string data_exp2_add_control();
std::string data_exp3_custom_dialog(const std::string& text);
std::string data_exp4_text_appearance();
std::string data_exp5_new_panel();
std::string data_exp_restore_all();
std::string data_exp_status_json();
