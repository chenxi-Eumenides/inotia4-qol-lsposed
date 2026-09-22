#include "game/adapter_ports/character_port.h"

#include <cstring>

#include "contracts/game_ports/ports.h"
#include "game/access/char.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult read_type(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = char_type(character);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_active_player_group(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!char_active_group_resolved()) return not_ready();
    *out = char_is_active_player_group(const_cast<void*>(character)) ? 1 : 0;
    return contracts::ok();
}

const contracts::QolCharacterPort kCharacterPort{
    contracts::kQolPortCharacterVersion,
    {&read_type, &read_active_player_group},
};

}  // namespace

const contracts::QolCharacterPort& character_port() { return kCharacterPort; }

}  // namespace qol::game
