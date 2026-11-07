#pragma once

#include <cstdint>

#include "contracts/plugin.h"

// 两个发起点的声明式资源表（owner="core"，kCallPatch；handler 为本 TU 的 wrapper）。
// 只声明不安装：宿主装配经 hooks 登记 + 机制分派安装；未就绪时不改写字节，可重试。
uint32_t save_enter_claim_count();
const qol::contracts::ResourceClaim* save_enter_claims();

namespace save_enter_detail {

struct SaveEnterPatchBinding {
    const char* callsite_symbol;
    const char* target_symbol;
    uint32_t original_call;
};

inline constexpr SaveEnterPatchBinding kResumeBinding{
    "F_SAVESLOT_SLOT_BUTTON_EXE_VMA", "F_GAME_START_RESUME_GAME_VMA", 0x97fecd56};
inline constexpr SaveEnterPatchBinding kNewGameConfirmBinding{
    "F_SELECTCHAR_BUTTON_START_EXE_VMA", "F_SELECT_CHARACTER_START_GAME_VMA", 0x97ffffea};

}  // namespace save_enter_detail
