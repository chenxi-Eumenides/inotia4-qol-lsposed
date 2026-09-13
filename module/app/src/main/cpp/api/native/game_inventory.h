#pragma once

#include <cstdint>
#include <string>

#include "core/native/stack_codec.h"
#include "data/native/item_class.h"

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
// item_count_encoding / item_is_equip 见 data/native/item_class.h（下沉后引用）。
// canonical 数量读取（与 stack_limit_enabled() 无关）：可堆叠类别直接按 S2 解码
// `+0x10` 数量位 128a+b（R-45），供 descriptor 物化/持久化等必须保存完整值的
// 路径使用；运行时展示/查询视图应改走 H-17 getter（关闭态按 R-47 决策 b 只返回
// b）。非可堆叠/类别不可用回退 getter 原版语义（装备返回 1）。
int canonical_item_count(void* item);
std::string build_inventory_json();
