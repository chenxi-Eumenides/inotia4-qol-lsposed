#pragma once

#include <string>

// 存档域（parse 域）：当前存档槽 + 存档槽列表。

std::string data_current_save_slot_json();
std::string data_save_slots_json();

// 存档操作：存档/进档/建档。
std::string data_op_save();
std::string data_op_enter_slot(int32_t slot);
std::string data_op_create_slot(int32_t slot, int32_t class_idx);
// 逻辑相位内联变体：供已在 kFramePointLogicPre 运行的 owner 命令使用（不再跨帧派发/阻塞）。
// 与上面两条共用 transition 单飞门，转换体完全相同（*_locked）。
std::string data_op_enter_slot_direct(int32_t slot);
std::string data_op_create_slot_direct(int32_t slot, int32_t class_idx);
