#include "game/adapter_ports/movement_input_port.h"

#include "contracts/game_ports/ports.h"
#include "game/input/eight_way_draw.h"
#include "game/input/eight_way_access.h"
#include "game/input/eight_way_move.h"
#include "game/input/eight_way_state.h"

namespace qol::game {
namespace {

const contracts::QolMovementInputPort kPort{
    contracts::kQolPortMovementInputVersion,
    {
        &qol::game::input::on_direction_touch,
        &qol::game::input::translate_press_key,
        &qol::game::input::char_move,
        &qol::game::input::draw_direction_indicator,
        &qol::game::input::set_enabled,
        &qol::game::input::reset,
        &qol::game::input::poll,
        &qol::game::input::handle_wait_coord,
        &qol::game::input::access::map_blocking_query,
    },
};

}  // namespace

const contracts::QolMovementInputPort& movement_input_port() { return kPort; }

}  // namespace qol::game
