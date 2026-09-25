#pragma once

#include "contracts/game_ports/character_state_port.h"

namespace qol::game::ops {

QolResult read_character_party(contracts::QolCharacterStateParty* out_party);
QolResult read_character_player(contracts::QolCharacterStatePlayer* out_player);
QolResult read_character_skills(contracts::QolCharacterStateSkills* out_skills);

}  // namespace qol::game::ops
