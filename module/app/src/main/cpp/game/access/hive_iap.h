#pragma once

#include <string>

// IAP 恢复（v0.5.18 hive 屏蔽恢复；D2c-2b-2 由 feature/patch/game_patch.cpp 迁入 game 层）。
//
// 语义：屏蔽 Com2uS Hive 阻断态——改写每日触发/HUD 门 GOT 指向的全局位，并调原版
// NetworkStore_SetState(0) + UIPopupMsg_SetProcessInfo(4,0)。属纯 game 状态操作，
// 不依赖 patch/资源机制。行为逐字保持。
std::string data_recover_after_hive_block();
