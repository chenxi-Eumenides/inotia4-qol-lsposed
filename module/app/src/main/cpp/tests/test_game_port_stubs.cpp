#include "contracts/game_ports/quest_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/world_port.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/save_call_port.h"
#include "contracts/game_ports/save_port.h"
#include "contracts/game_ports/craft_port.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/game_ports/custom_recipe_write_port.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/inventory_extras_port.h"
#include "contracts/game_ports/item_extras_port.h"
#include "contracts/game_ports/system_port.h"
#include "contracts/game_ports/save_write_port.h"
#include "contracts/game_ports/world_write_port.h"

namespace qol::game {
namespace {

const contracts::QolQuestPort kQuestPort{{1, 0}, {}, {}};
const contracts::QolWorldPort kWorldPort{{1, 0}, {}};
const contracts::QolItemPort kItemPort{{1, 0}, {}};
const contracts::QolCharacterPort kCharacterPort{{1, 0}, {}};
const contracts::QolCharacterHttpPort kCharacterHttpPort{{1, 0}, {}};
const contracts::QolChoicePort kChoicePort{{1, 0}, {}, {}};
const contracts::QolExtensionBagWritePort kExtensionBagWritePort{};
const contracts::QolSavePort kSavePort{{1, 0}, {}};
const contracts::QolSaveCallPort kSaveCallPort{{1, 0}, {}};
const contracts::QolCraftPort kCraftPort{{1, 0}, {}, {}};
const contracts::QolCraftUiWritePort kCraftUiWritePort{{1, 0}, nullptr, nullptr, nullptr,
                                                       nullptr, nullptr, nullptr, nullptr, nullptr,
                                                       nullptr, nullptr, nullptr};
const contracts::QolCustomRecipeWritePort kCustomRecipeWritePort{{1, 0}, nullptr, nullptr,
                                                                  nullptr};

const contracts::QolCharacterExtrasPort kCharacterExtrasPort{{1, 0}, {}};
const contracts::QolInventoryExtrasPort kInventoryExtrasPort{{1, 0}, {}};
const contracts::QolItemExtrasPort kItemExtrasPort{{1, 0}, {}};
const contracts::QolSystemPort kSystemPort{{1, 0}, {}};
const contracts::QolSaveWritePort kSaveWritePort{{1, 0}, {}};
const contracts::QolWorldWritePort kWorldWritePort{{1, 0}, {}};

}  // namespace

const contracts::QolQuestPort& quest_port() { return kQuestPort; }
const contracts::QolWorldPort& world_port() { return kWorldPort; }
const contracts::QolItemPort& item_port() { return kItemPort; }
const contracts::QolCharacterPort& character_port() { return kCharacterPort; }
const contracts::QolCharacterHttpPort& character_http_port() { return kCharacterHttpPort; }
const contracts::QolChoicePort& choice_port() { return kChoicePort; }
const contracts::QolExtensionBagWritePort& extension_bag_write_port() {
    return kExtensionBagWritePort;
}
const contracts::QolSavePort& save_port() { return kSavePort; }
const contracts::QolSaveCallPort& save_call_port() { return kSaveCallPort; }
const contracts::QolCraftPort& craft_port() { return kCraftPort; }
const contracts::QolCraftUiWritePort& craft_ui_write_port() { return kCraftUiWritePort; }
const contracts::QolCustomRecipeWritePort& custom_recipe_write_port() {
    return kCustomRecipeWritePort;
}
const contracts::QolCharacterExtrasPort& character_extras_port() {
    return kCharacterExtrasPort;
}
const contracts::QolInventoryExtrasPort& inventory_extras_port() {
    return kInventoryExtrasPort;
}
const contracts::QolItemExtrasPort& item_extras_port() { return kItemExtrasPort; }
const contracts::QolSystemPort& system_port() { return kSystemPort; }
const contracts::QolSaveWritePort& save_write_port() { return kSaveWritePort; }
const contracts::QolWorldWritePort& world_write_port() { return kWorldWritePort; }

}  // namespace qol::game
