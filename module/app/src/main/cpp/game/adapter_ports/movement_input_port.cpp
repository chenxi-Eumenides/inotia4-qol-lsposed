#include "game/adapter_ports/movement_input_port.h"

#include "contracts/game_ports/ports.h"
#include "game/input/eight_way_draw.h"
#include "game/input/eight_way_move.h"

namespace qol::game {
namespace {

const contracts::QolMovementInputPort kPort{
    contracts::kQolPortMovementInputVersion,
    {
        &qol::game::input::on_direction_touch,
        &qol::game::input::translate_press_key,
        &qol::game::input::char_move,
        &qol::game::input::draw_secondary_arrow,
        &qol::game::input::set_enabled,
        &qol::game::input::reset,
        &qol::game::input::poll,
    },
};

}  // namespace

const contracts::QolMovementInputPort& movement_input_port() { return kPort; }

}  // namespace qol::game
