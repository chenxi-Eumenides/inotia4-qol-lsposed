#pragma once

#include "contracts/game_ports/world_port.h"

namespace qol::game {

const contracts::QolWorldPort& world_port();

void world_port_set_map_exits_json(const char* json);

}  // namespace qol::game
