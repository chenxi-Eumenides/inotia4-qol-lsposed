#include "contracts/game_ports/quest_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/world_port.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/save_call_port.h"
#include "contracts/game_ports/save_port.h"

namespace qol::game {
namespace {

const contracts::QolQuestPort kQuestPort{{1, 0}, {}, {}};
const contracts::QolWorldPort kWorldPort{{1, 0}, {}};
const contracts::QolItemPort kItemPort{{1, 0}, {}};
const contracts::QolCharacterPort kCharacterPort{{1, 0}, {}};
const contracts::QolChoicePort kChoicePort{{1, 0}, {}, {}};
const contracts::QolExtensionBagWritePort kExtensionBagWritePort{};
const contracts::QolSavePort kSavePort{{1, 0}, {}};
const contracts::QolSaveCallPort kSaveCallPort{{1, 0}, {}};

}  // namespace

const contracts::QolQuestPort& quest_port() { return kQuestPort; }
const contracts::QolWorldPort& world_port() { return kWorldPort; }
const contracts::QolItemPort& item_port() { return kItemPort; }
const contracts::QolCharacterPort& character_port() { return kCharacterPort; }
const contracts::QolChoicePort& choice_port() { return kChoicePort; }
const contracts::QolExtensionBagWritePort& extension_bag_write_port() {
    return kExtensionBagWritePort;
}
const contracts::QolSavePort& save_port() { return kSavePort; }
const contracts::QolSaveCallPort& save_call_port() { return kSaveCallPort; }

}  // namespace qol::game
