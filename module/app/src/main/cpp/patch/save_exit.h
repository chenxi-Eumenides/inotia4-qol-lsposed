#pragma once

#include <cstdint>

#include "contracts/plugin.h"

// 退出存档（world -> 主菜单）事件发源地。
//
// 语义：从 world 返回主菜单时触发一次（含模块 go_main_menu 与游戏原生回主菜单，二者都走
// GAMESTATE_SetState(4) 的 state==4 分支）；不包含退档到选角、也不包含杀进程。发起点是
// 声明式 kCallPatch 资源（owner="core"，见 save_exit_claims），由宿主装配末段登记并经
// 机制分派安装：handler 先 publish kEventSaveExit（此时 world 数据仍有效）、再复刻原
// GAME_Exit。GAME_Exit 在 .text 内仅此一个调用点，故仅覆盖 world -> 主菜单。
//
// 本头文件纯逻辑、不依赖 Android，供 host 测试引用。

// 发起点声明式资源表（owner="core"，kCallPatch；handler 为本 TU 的 wrapper）。
// 只声明不安装：宿主装配末段经 hooks 登记 + 机制分派安装；未就绪时不改写字节，可重试。
uint32_t save_exit_claim_count();
const qol::contracts::ResourceClaim* save_exit_claims();
