#pragma once

#include "contracts/game_ports/movement_input_port.h"

namespace qol::game {

// 八向移动输入端口宿主实例（实现在 game/adapter_ports/movement_input_port.cpp）。
const contracts::QolMovementInputPort& movement_input_port();

}  // namespace qol::game
