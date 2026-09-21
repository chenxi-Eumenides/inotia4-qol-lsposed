#pragma once

#include <cstdint>

#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kWorldFactsCapabilityId = "world.facts";
constexpr QolVersion kWorldFactsVersion{1, 0};
constexpr uint16_t kWorldFactsVersionMajor = kWorldFactsVersion.major;
constexpr uint16_t kWorldFactsVersionMinor = kWorldFactsVersion.minor;

using WorldFactsStateFn = QolResult (*)(int32_t* out_state);
using WorldFactsGameStateGateFn = QolResult (*)(int32_t* out_open);
using WorldFactsMapRecordCountFn = QolResult (*)(int32_t* out_count);
using WorldFactsCurrentMapIdFn = QolResult (*)(int32_t* out_map_id);
using WorldFactsMapNameFn = QolResult (*)(int32_t map_id, const char** out_name);
using WorldFactsActivePlayerDirectionFn = QolResult (*)(int32_t* out_direction);
using WorldFactsMoneyFn = QolResult (*)(int64_t* out_money);
using WorldFactsTutorialStateFn = QolResult (*)(int32_t* out_state);

struct WorldFactsApi {
    WorldFactsStateFn state;
    WorldFactsGameStateGateFn game_state_gate;
    WorldFactsMapRecordCountFn map_record_count;
    WorldFactsCurrentMapIdFn current_map_id;
    WorldFactsMapNameFn map_name;
    WorldFactsActivePlayerDirectionFn active_player_direction;
    WorldFactsMoneyFn money;
    WorldFactsTutorialStateFn tutorial_state;
};

}  // namespace qol::contracts
