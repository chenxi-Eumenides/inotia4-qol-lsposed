#include "game/adapter_ports/combat_move_port.h"

#include "contracts/game_ports/ports.h"
#include "game/access/game_access.h"

namespace qol::game {
namespace {

int32_t is_active_player(void* character) {
    if (character == nullptr) return 0;
    const auto* bytes = static_cast<const uint8_t*>(character);
    return bytes[C_ACTIVE_PLAYER_FLAG] == 0 ? 1 : 0;
}

int32_t is_player_side(void* character) {
    if (character == nullptr) return 0;
    const auto* bytes = static_cast<const int8_t*>(character);
    return bytes[C_TYPE] == 0 ? 1 : 0;
}

void* target(void* character) {
    if (character == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(character) + C_MOVE_TARGET);
}

const void* current_action(void* character) {
    if (character == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(character) + C_ACTIVE_SKILL);
}

int32_t is_default_attack(void* character, const void* action) {
    return character == nullptr || action == nullptr || fn_char_is_default_attack == nullptr
               ? 0
               : fn_char_is_default_attack(character, const_cast<void*>(action));
}

int32_t is_walk_action(void* character, const void* action) {
    return character == nullptr || action == nullptr || fn_char_is_action_walk == nullptr
               ? 0
               : fn_char_is_action_walk(character, const_cast<void*>(action));
}

int32_t distance(void* a, void* b) {
    return a == nullptr || b == nullptr || fn_char_get_distance == nullptr
               ? 0
               : fn_char_get_distance(a, b);
}

int32_t action_range(void* character, const void* action) {
    return character == nullptr || action == nullptr || fn_char_get_act_distance == nullptr
               ? 0
               : fn_char_get_act_distance(character, const_cast<void*>(action));
}

int32_t is_no_move(void* character) {
    return character == nullptr || fn_char_is_no_move == nullptr
               ? 0
               : fn_char_is_no_move(character);
}

void set_target(void* character, void* value) {
    if (character != nullptr && fn_char_set_target != nullptr) fn_char_set_target(character, value);
}

const contracts::QolCombatMovePort kPort{
    contracts::kQolPortCombatMoveVersion,
    &is_active_player,
    &is_player_side,
    &target,
    &current_action,
    &is_default_attack,
    &is_walk_action,
    &distance,
    &action_range,
    &is_no_move,
    &set_target,
};

}  // namespace

const contracts::QolCombatMovePort& combat_move_port() { return kPort; }

}  // namespace qol::game
