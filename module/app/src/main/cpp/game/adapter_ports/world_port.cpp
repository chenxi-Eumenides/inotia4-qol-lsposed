#include "game/adapter_ports/world_port.h"

#include <cstring>

#include "contracts/game_ports/ports.h"
#include "game/access/world.h"
#include "game/state/game_state.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult read_state(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = world_state();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_game_state_gate(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = world_game_state_gate();
    if (value < 0) return not_ready();
    *out = value == 0 ? 1 : 0;
    return contracts::ok();
}

QolResult read_map_record_count(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = world_map_record_count();
    if (value <= 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_current_map_id(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    if (!world_ready()) return not_ready();
    *out = world_current_map_id();
    return contracts::ok();
}

QolResult read_map_name(int32_t map_id, const char** out) {
    if (map_id < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out = world_map_name(map_id);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_active_player_direction(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = world_active_player_direction();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_money(int64_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    if (!world_money_ready()) return not_ready();
    *out = world_money();
    return contracts::ok();
}

QolResult read_tutorial_state(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    *out = tutorial_state();
    return contracts::ok();
}

const contracts::QolWorldPort kWorldPort{
    contracts::kQolPortWorldVersion,
    {&read_state, &read_game_state_gate, &read_map_record_count, &read_current_map_id,
     &read_map_name, &read_active_player_direction, &read_money, &read_tutorial_state},
};

}  // namespace

const contracts::QolWorldPort& world_port() { return kWorldPort; }

}  // namespace qol::game
