#pragma once

#include <string>

// 任务域（parse 域）：当前/列表/已完成/已接任务。

int data_active_quest();
std::string data_quest_list_json();
std::string data_quest_completed_json();
std::string data_quest_active_json();

std::string data_op_quest_quit(int32_t quest_id);
