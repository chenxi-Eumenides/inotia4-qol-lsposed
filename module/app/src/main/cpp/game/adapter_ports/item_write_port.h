#pragma once

#include "contracts/game_ports/item_write_port.h"

namespace qol::game {

// item 写端口（私有，仅 core.item 可注入）：把固定宽度 POD 参数转交给
// game/state/item_http.h 声明的拼装实现（实现在 legacy 聚合 TU）。
const contracts::QolItemWritePort& item_write_port();

}  // namespace qol::game
