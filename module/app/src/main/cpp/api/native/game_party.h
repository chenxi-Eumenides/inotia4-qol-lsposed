#pragma once

#include <cstdint>
#include <string>

// 队伍域（parse 域）：队伍编成操作。

std::string data_op_party_swap(int32_t a, int32_t b);
std::string data_op_include_party(int mercenary_slot);
std::string data_op_exclude_party(int mercenary_slot);
std::string data_op_discharge(int mercenary_slot);
std::string data_op_withdraw(int mercenary_slot, int32_t equip_slot);
std::string data_op_switch_player(int32_t slot);

// 数据读取（read 域拆分）：队伍/佣兵 JSON 构造。
std::string build_party_json();
std::string build_mercenaries_json();
