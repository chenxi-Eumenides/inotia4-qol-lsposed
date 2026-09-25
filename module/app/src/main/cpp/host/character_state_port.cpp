#include "host/character_state_port.h"

#include "contracts/game_ports/ports.h"
#include "game/ops/character_state_read.h"

namespace qol::host {
namespace {

QolResult party(contracts::QolCharacterStateParty* out) {
    return qol::game::ops::read_character_party(out);
}

QolResult player(contracts::QolCharacterStatePlayer* out) {
    return qol::game::ops::read_character_player(out);
}

QolResult skills(contracts::QolCharacterStateSkills* out) {
    return qol::game::ops::read_character_skills(out);
}

const contracts::QolCharacterStatePort kPort{
    contracts::kQolPortCharacterStateVersion,
    {&party, &player, &skills},
};

}  // namespace

const contracts::QolCharacterStatePort& character_state_port() { return kPort; }

}  // namespace qol::host
