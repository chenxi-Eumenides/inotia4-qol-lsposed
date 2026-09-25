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
#include "game_inventory_basic.inc"
#include "game_inventory_equipment.inc"
#include "game_inventory_use.inc"
#include "game_inventory_read.inc"
#else
namespace {
std::string kernel_only_error() {
    return "{\"ok\":false,\"error\":\"kernel-only\"}";
}
}

std::string data_op_set_money(int64_t money) { (void)money; return kernel_only_error(); }
std::string data_op_add_money(int64_t delta) { (void)delta; return kernel_only_error(); }
std::string data_op_minus_money(int64_t delta) { (void)delta; return kernel_only_error(); }
std::string data_op_add_item(int32_t category, int32_t count, int32_t socket_total,
                             int32_t enhance_remaining, int32_t rarity_group) {
    (void)category;
    (void)count;
    (void)socket_total;
    (void)enhance_remaining;
    (void)rarity_group;
    return kernel_only_error();
}
std::string data_op_remove_item(int32_t category) { (void)category; return kernel_only_error(); }
std::string data_op_jewel(int role, int bag, int slot, int equip_slot) {
    (void)role;
    (void)bag;
    (void)slot;
    (void)equip_slot;
    return kernel_only_error();
}
std::string data_op_enchant(int role, int bag, int slot, int equip_slot) {
    (void)role;
    (void)bag;
    (void)slot;
    (void)equip_slot;
    return kernel_only_error();
}
std::string data_op_equip(int role, int bag, int slot) {
    (void)role;
    (void)bag;
    (void)slot;
    return kernel_only_error();
}
std::string data_op_unequip(int role, int32_t equip_slot) {
    (void)role;
    (void)equip_slot;
    return kernel_only_error();
}
std::string data_op_use_item(int bag, int slot) {
    (void)bag;
    (void)slot;
    return kernel_only_error();
}
std::string data_op_dice_accept() { return kernel_only_error(); }
std::string data_op_dice_reject() { return kernel_only_error(); }
std::string data_op_discard_item(int bag, int slot) {
    (void)bag;
    (void)slot;
    return kernel_only_error();
}
std::string data_op_sell_item(int bag, int slot) {
    (void)bag;
    (void)slot;
    return kernel_only_error();
}
std::string data_op_move_item(int bag, int slot, int count, int to_bag, int to_slot) {
    (void)bag;
    (void)slot;
    (void)count;
    (void)to_bag;
    (void)to_slot;
    return kernel_only_error();
}
void append_item_attrs(std::string& s, void* item) {
    (void)s;
    (void)item;
}
std::string build_inventory_json() { return "{\"bags\":[]}"; }
std::string data_debug_item_raw_json(int32_t bag, int32_t slot) {
    (void)bag;
    (void)slot;
    return kernel_only_error();
}
#endif
