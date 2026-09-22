#include "contracts/game_ports/quest_port.h"

namespace qol::game {
namespace {

const contracts::QolQuestPort kQuestPort{{1, 0}, {}, {}};

}  // namespace

const contracts::QolQuestPort& quest_port() { return kQuestPort; }

}  // namespace qol::game
