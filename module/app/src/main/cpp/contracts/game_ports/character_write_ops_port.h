#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// 角色/战斗/成长/队伍写面的 typed 低层原语端口（私有，仅 core.character 可注入）。
// 每个函数对应一个 QolCharacterWriteKind：入参为固定宽度标量，不接收 kind、不返回 JSON、
// 不使用 std::string。成功返回 ok；失败返回非 ok，并把迁移前 op_err 文案写入 out->error_message，
// 由 owner（core.character）按迁移前信封拼装响应。
// 当前 20 个 kind 都只需回传成功/失败与错误文案，无需回传数值；出现需回传数值的 kind 时再扩结果体。
constexpr uint32_t kQolCharacterWriteOpErrorCapacity = 128;

struct QolCharacterWriteOpResult {
    uint32_t error_message_len = 0;
    char error_message[kQolCharacterWriteOpErrorCapacity]{};
};

using QolCharacterWriteOp1Fn = QolResult (*)(int32_t a, QolCharacterWriteOpResult* out);
using QolCharacterWriteOp2Fn = QolResult (*)(int32_t a, int32_t b, QolCharacterWriteOpResult* out);
using QolCharacterWriteOp3Fn = QolResult (*)(int32_t a, int32_t b, int32_t c,
                                             QolCharacterWriteOpResult* out);
using QolCharacterWriteOpExpFn = QolResult (*)(int32_t role, int64_t exp,
                                               QolCharacterWriteOpResult* out);

struct QolCharacterWriteOpsPort {
    QolVersion abi_version;
    QolCharacterWriteOp2Fn set_hp;            // i0=role, i1=hp
    QolCharacterWriteOp2Fn set_mp;            // i0=role, i1=mp
    QolCharacterWriteOp3Fn set_level;         // i0=role, i1=level, i2=force(0/1)
    QolCharacterWriteOpExpFn set_experience;  // i0=role, i64=exp
    QolCharacterWriteOp3Fn set_attr;          // i0=role, i1=attr_index, i2=value
    QolCharacterWriteOp2Fn set_status_point;  // i0=role, i1=points
    QolCharacterWriteOp2Fn party_swap;        // i0=slot_a, i1=slot_b
    QolCharacterWriteOp2Fn set_auto_attack;   // i0=role, i1=on(0/1)
    QolCharacterWriteOp2Fn cast_skill;        // i0=role, i1=action_id
    QolCharacterWriteOp2Fn attack_target;     // i0=role, i1=target_slot
    QolCharacterWriteOp1Fn switch_player;     // i0=slot
    QolCharacterWriteOp1Fn stop_combat;       // i0=role
    QolCharacterWriteOp3Fn learn_action;      // i0=role, i1=action_id, i2=level
    QolCharacterWriteOp2Fn add_stat;          // i0=role, i1=attr_index（单点 +1）
    QolCharacterWriteOp1Fn stat_reset;        // i0=role
    QolCharacterWriteOp1Fn skill_reset;       // i0=role
    QolCharacterWriteOp1Fn party_include;     // i0=merc_slot
    QolCharacterWriteOp1Fn party_exclude;     // i0=merc_slot
    QolCharacterWriteOp1Fn party_discharge;   // i0=merc_slot
    QolCharacterWriteOp2Fn party_withdraw;    // i0=merc_slot, i1=equip_slot
};

static_assert(std::is_standard_layout<QolCharacterWriteOpResult>::value,
              "QolCharacterWriteOpResult must remain standard layout");
static_assert(std::is_trivially_copyable<QolCharacterWriteOpResult>::value,
              "QolCharacterWriteOpResult must remain trivially copyable");
static_assert(std::is_standard_layout<QolCharacterWriteOpsPort>::value,
              "QolCharacterWriteOpsPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCharacterWriteOpsPort>::value,
              "QolCharacterWriteOpsPort must remain trivially copyable");

}  // namespace qol::contracts
