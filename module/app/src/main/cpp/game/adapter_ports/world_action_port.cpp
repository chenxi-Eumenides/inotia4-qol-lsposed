#include "game/adapter_ports/world_action_port.h"

#include "contracts/game_ports/ports.h"

namespace qol::game {

const contracts::QolWorldActionPort& world_action_port() {
    static const contracts::QolWorldActionPort port{
        contracts::kQolPortWorldActionVersion, game_world_action_api()};
    return port;
}

}  // namespace qol::game
