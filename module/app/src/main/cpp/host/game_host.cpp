#include "host/adapters.h"

#include "game/adapter_ports/game_host_port.h"

namespace qol::host {

extern const contracts::QolGameHost g_game_host = qol::game::game_host_adapter();

void game_host_mark_main_thread() { qol::game::game_host_mark_main_thread(); }

}  // namespace qol::host
