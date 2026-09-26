#include "game_inventory.h"
#include "game/access/game_access.h"
#include "game/state/game_state.h"
#include "game/ops/game_ops_common.h"
#include "game/ops/item_dice.h"
#include "game/ops/item_inventory_state_write.h"
#include "game/ops/item_money.h"
#include "game/adapter_ports/item_count_facade.h"
#include "contracts/extension_bag_port.h"
#include "core/native/inventory_trade.h"
#include "infra/log/qol_log.h"
#include <cstdint>
#include <string>

#if !QOL_KERNEL_ONLY
#include "game_inventory_read.inc"
#else
namespace {
std::string kernel_only_error() {
    return "{\"ok\":false,\"error\":\"kernel-only\"}";
}
}

void append_item_attrs(std::string& s, void* item) {
    (void)s;
    (void)item;
}
std::string build_inventory_json() { return "{\"bags\":[]}"; }
#endif
