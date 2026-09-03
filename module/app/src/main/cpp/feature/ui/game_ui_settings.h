#pragma once

#include <cstdint>
#include <string>
#include <jni.h>

// 模块设置 UI 域（ui-settings v0.6.9）：钩住主菜单「更多游戏」按钮 ExecuteProc
// → 点击打开模块设置面板（3 布尔开关可切换 + 监听地址/端口只读显示），不再用 OPTION 入口。
// 机制：注入 INAP_GEMSHOP 死条目 state（exp6 先例）+ PtrHook 覆盖更多游戏按钮 ExecuteProc
// （更多游戏按钮槽 0x3099f8，原 ExecuteProc 0x151e38→GotoShowMoreGames 打开网页）
// + process 内 LCD/GRPX 绘制（exp6 验证色块）+ 半透明面板透出主菜单背景。
// 参考 docs/system/ui.md §6 + ui-experiments.md（exp6 文字解法：font=1 官方实证）。
// 链路：懒注入线程轮询主菜单 screen → PtrHook 覆盖更多游戏 ExecuteProc →
// 点击 → UI_SetPopupProcessInfo(1, injected_state_id) 打开自定义面板。

std::string data_settings_ui_inject();      // 启用注入（懒注入线程启动 + state 注入）
std::string data_settings_ui_status_json(); // 状态 JSON（注入标志/面板状态/配置项/按钮矩形）
std::string data_settings_ui_restore();     // 还原 state 条目 + 卸载按钮 hook + 清面板
std::string data_settings_ui_open_panel();  // 调试：直接打开模块设置面板
std::string data_settings_ui_open_option(); // 调试：打开主菜单环境设置（模拟点击环境设置按钮）
void settings_ui_start_auto_inject();       // 游戏进程初始化后自动启动懒注入线程

void settings_register_config_bridge(JNIEnv* env, jclass bridge_class);
