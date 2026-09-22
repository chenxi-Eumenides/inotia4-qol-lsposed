#pragma once

#include <cstdint>

#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kSaveFactsCapabilityId = "save.facts";
constexpr QolVersion kSaveFactsVersion{1, 0};
constexpr uint16_t kSaveFactsVersionMajor = kSaveFactsVersion.major;
constexpr uint16_t kSaveFactsVersionMinor = kSaveFactsVersion.minor;

using SaveFactsSlotStateFn = QolResult (*)(int32_t slot, int32_t* out_state);
using SaveFactsSlotExistsFn = QolResult (*)(int32_t slot, int32_t* out_exists);
using SaveFactsSlotHeroIndexFn = QolResult (*)(int32_t slot, int32_t* out_index);
using SaveFactsHeroLevelFn = QolResult (*)(int32_t slot, int32_t* out_level);
using SaveFactsHeroClassFn = QolResult (*)(int32_t slot, int32_t* out_class);
using SaveFactsLoadDataFn = QolResult (*)(int32_t slot, void** out_data, int32_t* out_size);
using SaveFactsHubKeyFn = QolResult (*)(const char** out_key);
using SaveFactsMapIdFn = QolResult (*)(int32_t slot, int32_t* out_map_id);
using SaveFactsCurrentSlotFn = QolResult (*)(int32_t* out_slot);
using SaveFactsIsOkFn = QolResult (*)(int32_t* out_ok);

struct SaveFactsApi {
    SaveFactsSlotStateFn slot_state;
    SaveFactsSlotExistsFn slot_exists;
    SaveFactsSlotHeroIndexFn slot_hero_index;
    SaveFactsHeroLevelFn hero_level;
    SaveFactsHeroClassFn hero_class;
    SaveFactsLoadDataFn load_data;
    SaveFactsHubKeyFn hub_key;
    SaveFactsMapIdFn map_id;
    SaveFactsCurrentSlotFn current_slot;
    SaveFactsIsOkFn is_ok;
};

}  // namespace qol::contracts
