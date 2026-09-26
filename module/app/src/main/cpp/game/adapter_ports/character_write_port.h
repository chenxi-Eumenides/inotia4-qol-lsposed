#pragma once

#include "contracts/game_ports/character_write_ops_port.h"
#include "contracts/game_ports/character_write_port.h"

namespace qol::game {

// character 写端口（私有，仅 core.character 可注入）：把固定宽度 POD 参数转交给
// game/state/character_http.h 声明的拼装实现（实现在 legacy 聚合 TU）。
const contracts::QolCharacterWritePort& character_write_port();

// character 写面的 typed 低层原语端口：绑定 game/ops/character_write 的 20 个原语，
// owner 自行按 kind 分派并组装响应。
const contracts::QolCharacterWriteOpsPort& character_write_ops_port();

}  // namespace qol::game
