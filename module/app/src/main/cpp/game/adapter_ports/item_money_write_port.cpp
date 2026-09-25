#include "game/adapter_ports/item_money_write_port.h"

#include "game/ops/item_money.h"

namespace qol::game {
namespace {

const contracts::QolItemMoneyWritePort kPort{
    contracts::kQolPortItemMoneyWriteVersion,
    &ops::set_money,
};

}  // namespace

const contracts::QolItemMoneyWritePort& item_money_write_port() { return kPort; }

}  // namespace qol::game
