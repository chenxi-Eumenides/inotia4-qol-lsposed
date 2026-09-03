#pragma once

#include <string>

// 存档域（parse 域）：当前存档槽 + 存档槽列表。

std::string data_current_save_slot_json();
std::string data_save_slots_json();

// 存档操作：存档/进档/建档。
std::string data_op_save();
std::string data_op_enter_slot(int32_t slot);
std::string data_op_create_slot(int32_t slot, int32_t class_idx);
