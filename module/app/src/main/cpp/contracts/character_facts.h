#pragma once

#include <cstdint>

#include "game_host.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kCharacterFactsCapabilityId = "character.facts";
constexpr QolVersion kCharacterFactsVersion{1, 0};
constexpr uint16_t kCharacterFactsVersionMajor = kCharacterFactsVersion.major;
constexpr uint16_t kCharacterFactsVersionMinor = kCharacterFactsVersion.minor;

using CharacterFactsTypeFn = QolResult (*)(const void* character, int32_t* out_type);
using CharacterFactsIsActivePlayerGroupFn = QolResult (*)(const void* character,
                                                           int32_t* out_active);

struct CharacterFactsApi {
    CharacterFactsTypeFn type;
    CharacterFactsIsActivePlayerGroupFn is_active_player_group;
};

}  // namespace qol::contracts
