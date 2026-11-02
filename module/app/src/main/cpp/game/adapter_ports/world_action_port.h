#pragma once

#include "contracts/game_ports/world_action_port.h"

namespace qol::game {

// 世界动作 thunk 的唯一实现入口（定义在 game_host_port.cpp，与 QolGameHost::world_action 共用）。
const contracts::QolWorldActionApi& game_world_action_api();

const contracts::QolWorldActionPort& world_action_port();

}  // namespace qol::game
