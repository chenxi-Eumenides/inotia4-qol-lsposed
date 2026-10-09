#pragma once

#include "contracts/game_ports/craft_port.h"

namespace qol::game {

const contracts::QolCraftPort& craft_port();
QolResult craft_stuff_slot_index(void* transient_control, int32_t* out_index);

}  // namespace qol::game
