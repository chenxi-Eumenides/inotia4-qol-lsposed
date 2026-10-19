#pragma once

#include <cstdint>

#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kWorldFactsCapabilityId = "world.facts";
constexpr QolVersion kWorldFactsVersion{2, 1};
constexpr uint16_t kWorldFactsVersionMajor = kWorldFactsVersion.major;
constexpr uint16_t kWorldFactsVersionMinor = kWorldFactsVersion.minor;

using WorldFactsStateFn = QolResult (*)(int32_t* out_state);
using WorldFactsGameStateGateFn = QolResult (*)(int32_t* out_open);
using WorldFactsMapRecordCountFn = QolResult (*)(int32_t* out_count);
using WorldFactsCurrentMapIdFn = QolResult (*)(int32_t* out_map_id);
using WorldFactsMapNameFn = QolResult (*)(int32_t map_id, const char** out_name);
using WorldFactsActivePlayerDirectionFn = QolResult (*)(int32_t* out_direction);
using WorldFactsTutorialStateFn = QolResult (*)(int32_t* out_state);
// 变体序列（序数与 game/variants GameSeries 枚举逐位对齐，由 game/adapter_ports
// 的 static_assert 锚定）。未初始化/未知返回 kWorldFactsSeriesUnknown。
using WorldFactsSeriesFn = QolResult (*)(int32_t* out_series);

constexpr int32_t kWorldFactsSeriesUnknown = 0;
constexpr int32_t kWorldFactsSeriesOriginal = 1;
constexpr int32_t kWorldFactsSeriesOverhaul = 2;
constexpr int32_t kWorldFactsSeriesMonster = 3;
constexpr int32_t kWorldFactsSeriesC18 = 4;

struct WorldFactsApi {
    WorldFactsStateFn state;
    WorldFactsGameStateGateFn game_state_gate;
    WorldFactsMapRecordCountFn map_record_count;
    WorldFactsCurrentMapIdFn current_map_id;
    WorldFactsMapNameFn map_name;
    WorldFactsActivePlayerDirectionFn active_player_direction;
    WorldFactsTutorialStateFn tutorial_state;
    WorldFactsSeriesFn series;
};

}  // namespace qol::contracts
