#include "game/adapter_ports/character_http_port.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "contracts/game_ports/ports.h"
#include "game/state/character_http.h"
#include "game/adapter_ports/game_host_port.h"

// player 体直接复用既有缓存读取器（core/native/game_cache.h 的 data_player_json）。
std::string data_player_json();

namespace qol::game {
namespace {

QolResult copy_json(const std::string& json, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || json.size() >= capacity) {
        return contracts::error(kQolErrBlocked, "http response buffer too small", 31);
    }
    std::memcpy(out, json.data(), json.size());
    out[json.size()] = '\0';
    *out_size = static_cast<uint32_t>(json.size());
    return contracts::ok();
}

QolResult body_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (!game_host_is_main_thread()) {
        return contracts::error(kQolErrBlocked, "wrong thread", 12);
    }
    switch (static_cast<contracts::QolCharacterHttpJsonKind>(kind)) {
        case contracts::QolCharacterHttpJsonKind::kPlayer:
            return copy_json(data_player_json(), out, capacity, out_size);
        case contracts::QolCharacterHttpJsonKind::kParty:
            return copy_json(build_character_party_json(), out, capacity, out_size);
        case contracts::QolCharacterHttpJsonKind::kSkills:
            return copy_json(build_character_skills_json(), out, capacity, out_size);
        case contracts::QolCharacterHttpJsonKind::kMercenaries:
            return copy_json(build_character_mercenaries_json(), out, capacity, out_size);
    }
    return contracts::error(kQolErrInvalidArgument, "unknown character json kind", 28);
}

const contracts::QolCharacterHttpPortReadApi kRead{&body_json};
const contracts::QolCharacterHttpPort kPort{contracts::kQolPortCharacterHttpVersion, kRead};

}  // namespace

const contracts::QolCharacterHttpPort& character_http_port() { return kPort; }

}  // namespace qol::game
