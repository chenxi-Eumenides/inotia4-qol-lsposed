#pragma once

#include "contracts/game_ports/world_write_port.h"

namespace qol::game {

// 世界写适配端口的宿主实例（实现在 game/adapter_ports/world_write_port.cpp）。
// 仅 world_teleport owner 可经插件描述符注入（见 host/plugin_registry.cpp 的私有端口约束）。
const contracts::QolWorldWritePort& world_write_port();

}  // namespace qol::game
