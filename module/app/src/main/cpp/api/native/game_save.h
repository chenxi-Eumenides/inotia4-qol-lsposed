#pragma once

#include <string>

// 存档域（parse 域）：当前存档槽 + 存档槽列表。

std::string data_current_save_slot_json();
std::string data_save_slots_json();

// 存档操作（进档/建档已迁 game/access/save.{h,cpp}；仅保留 dialog 面仍在消费的 save 薄转发）。
std::string data_op_save();
