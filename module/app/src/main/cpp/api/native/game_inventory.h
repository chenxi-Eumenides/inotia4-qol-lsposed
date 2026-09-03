#pragma once

#include <cstdint>
#include <string>

// 背包域（parse 域）：背包/装备/金钱操作。

std::string data_op_set_money(int64_t money);
std::string data_op_add_money(int64_t delta);
std::string data_op_minus_money(int64_t delta);
std::string data_op_add_item(int32_t category, int32_t count);
std::string data_op_remove_item(int32_t category);

std::string data_op_jewel(int role, int bag, int slot, int equip_slot);
std::string data_op_enchant(int role, int bag, int slot, int equip_slot);
std::string data_op_equip(int role, int bag, int slot);
std::string data_op_unequip(int role, int32_t equip_slot);
std::string data_op_use_item(int bag, int slot);
std::string data_op_dice_accept();
std::string data_op_dice_reject();
std::string data_op_discard_item(int bag, int slot);
std::string data_op_sell_item(int bag, int slot);
std::string data_op_move_item(int bag, int slot, int count, int to_bag, int to_slot);

// 数据读取（read 域拆分）：物品属性/装备判定/背包 JSON 构造。
void append_item_attrs(std::string& s, void* item);
bool item_is_equip(void* item);
std::string build_inventory_json();
