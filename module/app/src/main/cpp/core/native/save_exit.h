#pragma once

#include <cstdint>

// 退出存档（world -> 主菜单）回调。
//
// 语义：从 world 返回主菜单时触发一次（含模块 go_main_menu 与游戏原生回主菜单，二者都走
// GAMESTATE_SetState(4) 的 state==4 分支）；不包含退档到选角、也不包含杀进程。发起点用
// call_patch 改指 wrapper：先派发回调（此时 world 数据仍有效）、再复刻原 GAME_Exit。
//
// 线程模型：register/unregister 任意线程可调；回调在游戏主线程执行。
// 本头文件纯逻辑、不依赖 Android，供 host 测试引用。

using SaveExitFn = void (*)(void* ctx);

// 注册退出存档回调（同一 fn+ctx 幂等）；fn 为空返回 false。
bool save_exit_register(SaveExitFn fn, void* ctx);

// 注销退出存档回调（移除所有匹配的 fn+ctx）；幂等。
void save_exit_unregister(SaveExitFn fn, void* ctx);

// 安装退出存档发起点（GAMESTATE_SetState+0xb0 bl GAME_Exit）的 call_patch；幂等，失败 fail-closed。
bool save_exit_host_install_if_ready();
