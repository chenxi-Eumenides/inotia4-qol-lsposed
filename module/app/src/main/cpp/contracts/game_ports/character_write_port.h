#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// 角色/战斗/成长/队伍写面的私有适配端口（阶段 5 写面收口）：仅 core.character 可注入。
// 参数统一用固定宽度 POD（QolCharacterWriteArgs）；数值语义见各 kind 注释。
enum class QolCharacterWriteKind : int32_t {
    kSetHp = 0,           // i0=role, i1=hp
    kSetMp = 1,           // i0=role, i1=mp
    kSetLevel = 2,        // i0=role, i1=level, i2=force(0/1)
    kSetExperience = 3,   // i0=role, i64=exp
    kSetAttr = 4,         // i0=role, i1=attr_index, i2=value（单点；批量由调用方循环）
    kSetStatusPoint = 5,  // i0=role, i1=points
    kPartySwap = 6,       // i0=slot_a, i1=slot_b
    kSetAutoAttack = 7,   // i0=role, i1=on(0/1)
    kCastSkill = 8,       // i0=role, i1=action_id
    kAttackTarget = 9,    // i0=role, i1=target_slot
    kSwitchPlayer = 10,   // i0=slot
    kStopCombat = 11,     // i0=role
    kLearnAction = 12,    // i0=role, i1=action_id, i2=level
    kAddStat = 13,        // i0=role, i1=attr_index（单点 +1；批量由调用方循环）
    kStatReset = 14,      // i0=role
    kSkillReset = 15,     // i0=role
    kPartyInclude = 16,   // i0=merc_slot
    kPartyExclude = 17,   // i0=merc_slot
    kPartyDischarge = 18, // i0=merc_slot
    kPartyWithdraw = 19,  // i0=merc_slot, i1=equip_slot
};

struct QolCharacterWriteArgs {
    int32_t kind = 0;
    int32_t i0 = 0;
    int32_t i1 = 0;
    int32_t i2 = 0;
    int32_t i3 = 0;
    int64_t i64 = 0;
};

// 写函数：主线程执行；成功写 out（迁移前原响应 JSON）并置 out_size，失败返回非 ok。
using QolCharacterWriteFn = QolResult (*)(const QolCharacterWriteArgs* args, char* out,
                                         uint32_t capacity, uint32_t* out_size);

struct QolCharacterWritePortReadApi {
    QolCharacterWriteFn write;
};

struct QolCharacterWritePort {
    QolVersion abi_version;
    QolCharacterWritePortReadApi read;
};

static_assert(std::is_standard_layout<QolCharacterWritePort>::value,
              "QolCharacterWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCharacterWritePort>::value,
              "QolCharacterWritePort must remain trivially copyable");

}  // namespace qol::contracts
