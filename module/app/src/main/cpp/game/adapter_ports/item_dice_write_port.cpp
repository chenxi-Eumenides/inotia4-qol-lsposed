#include "game/adapter_ports/item_dice_write_port.h"

#include "game/ops/item_dice.h"

namespace qol::game {
namespace {

const contracts::QolItemDiceWritePort kPort{
    contracts::kQolPortItemDiceWriteVersion,
    &ops::reject_dice,
};

}  // namespace

const contracts::QolItemDiceWritePort& item_dice_write_port() { return kPort; }

}  // namespace qol::game
