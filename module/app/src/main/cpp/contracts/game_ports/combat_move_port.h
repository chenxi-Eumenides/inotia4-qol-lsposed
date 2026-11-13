#pragma once

#include <cstdint>
#include <type_traits>

#include "../version.h"

namespace qol::contracts {

struct QolCombatMovePort {
    QolVersion abi_version;
    int32_t (*is_active_player)(void* character);
    int32_t (*is_player_side)(void* character);
    void* (*target)(void* character);
    const void* (*current_action)(void* character);
    int32_t (*is_default_attack)(void* character, const void* action);
    int32_t (*is_walk_action)(void* character, const void* action);
    int32_t (*distance)(void* a, void* b);
    int32_t (*action_range)(void* character, const void* action);
    int32_t (*is_no_move)(void* character);
    void (*set_target)(void* character, void* target);
};

static_assert(std::is_standard_layout<QolCombatMovePort>::value,
              "QolCombatMovePort must be standard-layout");
static_assert(std::is_trivially_copyable<QolCombatMovePort>::value,
              "QolCombatMovePort must be trivially copyable");

}  // namespace qol::contracts
