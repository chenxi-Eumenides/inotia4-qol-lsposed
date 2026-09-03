#pragma once

#include <cstdint>
#include <string>

// 角色/战斗域（parse 域）：战斗操作 + 角色数值写操作。

std::string data_op_cast(int role, int32_t action_id);
std::string data_op_attack(int role, int target_slot);
std::string data_op_stop_combat(int role);
std::string data_op_stat_reset(int role);
std::string data_op_skill_reset(int role);

std::string data_op_set_experience(int role, int64_t exp);
std::string data_op_set_level(int role, int32_t level, bool force);
std::string data_op_add_experience(int role, int64_t delta);
std::string data_op_set_status_point(int role, int32_t points);
std::string data_op_add_stat(int role, int32_t attr);
std::string data_op_set_auto_attack(int role, int32_t onoff);
std::string data_op_set_skill_usage(int role, int32_t onoff);
std::string data_op_learn_action(int role, int32_t action_id, int32_t level);
std::string data_op_set_hp(int role, int32_t hp);
std::string data_op_set_mp(int role, int32_t mp);
std::string data_op_set_attr(int role, int attr_index, int32_t value);

// 数据读取（read 域拆分）：角色/技能 JSON 构造。
std::string member_json(void* ch);
std::string build_player_json();
std::string build_skills_json();
