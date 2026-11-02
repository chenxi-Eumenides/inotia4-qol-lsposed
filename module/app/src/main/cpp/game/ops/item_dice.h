#pragma once

#include "contracts/game_ports/item_dice_write_port.h"
#include "contracts/result.h"

namespace qol::game::ops {

QolResult reject_dice();
// 确认并应用待处理骰子结果；out 只填固定宽度数值，不构造 JSON（由 owner 组装）。
QolResult accept_dice(contracts::QolItemDiceAcceptResult* out);

}  // namespace qol::game::ops
