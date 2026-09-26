#pragma once

#include "contracts/game_ports/character_write_ops_port.h"

namespace qol::game::ops {

// 20 个 character 写 kind 的 typed 低层原语：逐条等价迁移 api/native/game_character.cpp 与
// api/native/game_party.cpp 的 data_op_*（成功时 frame_cache_force_refresh 恰一次，失败时回传
// 原 op_err 文案）。不含 JSON、不接收 kind、不使用 std::string；owner 负责信封与 state 附加。
QolResult set_hp(int32_t role, int32_t hp, contracts::QolCharacterWriteOpResult* out);
QolResult set_mp(int32_t role, int32_t mp, contracts::QolCharacterWriteOpResult* out);
QolResult set_level(int32_t role, int32_t level, int32_t force,
                    contracts::QolCharacterWriteOpResult* out);
QolResult set_experience(int32_t role, int64_t exp, contracts::QolCharacterWriteOpResult* out);
QolResult set_attr(int32_t role, int32_t attr_index, int32_t value,
                   contracts::QolCharacterWriteOpResult* out);
QolResult set_status_point(int32_t role, int32_t points,
                           contracts::QolCharacterWriteOpResult* out);
QolResult party_swap(int32_t a, int32_t b, contracts::QolCharacterWriteOpResult* out);
QolResult set_auto_attack(int32_t role, int32_t onoff,
                          contracts::QolCharacterWriteOpResult* out);
QolResult cast_skill(int32_t role, int32_t action_id, contracts::QolCharacterWriteOpResult* out);
QolResult attack_target(int32_t role, int32_t target_slot,
                        contracts::QolCharacterWriteOpResult* out);
QolResult switch_player(int32_t slot, contracts::QolCharacterWriteOpResult* out);
QolResult stop_combat(int32_t role, contracts::QolCharacterWriteOpResult* out);
QolResult learn_action(int32_t role, int32_t action_id, int32_t level,
                       contracts::QolCharacterWriteOpResult* out);
QolResult add_stat(int32_t role, int32_t attr_index, contracts::QolCharacterWriteOpResult* out);
QolResult stat_reset(int32_t role, contracts::QolCharacterWriteOpResult* out);
QolResult skill_reset(int32_t role, contracts::QolCharacterWriteOpResult* out);
QolResult party_include(int32_t mercenary_slot, contracts::QolCharacterWriteOpResult* out);
QolResult party_exclude(int32_t mercenary_slot, contracts::QolCharacterWriteOpResult* out);
QolResult party_discharge(int32_t mercenary_slot, contracts::QolCharacterWriteOpResult* out);
QolResult party_withdraw(int32_t mercenary_slot, int32_t equip_slot,
                         contracts::QolCharacterWriteOpResult* out);

}  // namespace qol::game::ops
