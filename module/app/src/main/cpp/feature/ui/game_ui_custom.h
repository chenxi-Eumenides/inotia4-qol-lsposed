#pragma once

#include <cstdint>
#include <string>

// 自定义 UI 面板域（ui-custom v0.6.8）：替换商店返回按钮 + 注入完全自定义面板。
// 参考 docs/system/ui.md §6（5 种自定义 UI 方式）+ game_ui_exp.cpp（exp1/exp5 先例）。
// 链路：商店返回按钮 [0x308000+0xe0] ExecuteProc → PtrHook 覆盖 → 点击打开自定义面板
//（state list daily_reward 条目注入 enter/process/f3/f4/event）。

std::string data_custom_btn_inject();      // 替换商店返回按钮（懒注入，轮询槽非空）
std::string data_custom_panel_open();      // 手动打开自定义面板（调试用，等价点击替换按钮）
std::string data_custom_restore();         // 还原商店按钮 + 还原 state 条目 + 清控件树
std::string data_custom_status_json();     // 状态 JSON（注入标志/按钮指针/面板状态）
