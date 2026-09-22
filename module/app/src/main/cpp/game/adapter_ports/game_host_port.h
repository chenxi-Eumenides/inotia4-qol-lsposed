#pragma once

#include "contracts/game_host.h"

namespace qol::game {

const contracts::QolGameHost& game_host_adapter();
void game_host_mark_main_thread();

}  // namespace qol::game
