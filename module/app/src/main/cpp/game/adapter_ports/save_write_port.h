#pragma once

#include "contracts/game_ports/save_write_port.h"

namespace qol::game {

// save_backup owner 的存档写端口（game 层适配，实现在 legacy 聚合 TU）。
const contracts::QolSaveWritePort& save_write_port();

}  // namespace qol::game
