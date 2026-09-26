#pragma once

#include <cstdint>
#include <string>

// 角色/战斗域（parse 域）：战斗操作 + 角色数值写操作。

std::string data_op_add_experience(int role, int64_t delta);
std::string data_op_set_skill_usage(int role, int32_t onoff);

// 数据读取（read 域拆分）：角色/技能 JSON 构造。
std::string member_json(void* ch);
std::string build_player_json();
std::string build_skills_json();
