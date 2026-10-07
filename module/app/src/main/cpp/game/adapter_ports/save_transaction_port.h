#pragma once

#include "contracts/game_ports/save_transaction_port.h"

namespace qol::game {

const contracts::QolSaveTransactionPort& save_transaction_port();

// 装配期：解析 SAVE_LoadCharacterAll 内部到 SAVE_Save 的唯一 BL，建立 loading 旁路
// 合法返回点；解析 SAVE_GetSaveFileName 注入相对名缝。解析失败时旁路与证据路径保持
// 不可用（NotReady/Blocked），不回退裸地址。
void save_transaction_install();

}  // namespace qol::game
