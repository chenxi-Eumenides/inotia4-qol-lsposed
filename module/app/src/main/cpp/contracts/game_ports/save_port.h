#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "ports.h"
#include "../version.h"

namespace qol::contracts {

struct QolSavePortReadApi {
    QolResult (*slot_state)(int32_t slot, int32_t* out_state);
    QolResult (*slot_exists)(int32_t slot, int32_t* out_exists);
    QolResult (*slot_hero_index)(int32_t slot, int32_t* out_index);
    QolResult (*hero_level)(int32_t slot, int32_t* out_level);
    QolResult (*hero_class)(int32_t slot, int32_t* out_class);
    QolResult (*load_data)(int32_t slot, void** out_data, int32_t* out_size);
    QolResult (*hub_key)(const char** out_key);
    QolResult (*map_id)(int32_t slot, int32_t* out_map_id);
    QolResult (*current_slot)(int32_t* out_slot);
    QolResult (*is_ok)(int32_t* out_ok);
};

struct QolSavePort {
    QolVersion abi_version;
    QolSavePortReadApi read;
};

static_assert(std::is_standard_layout<QolSavePort>::value,
              "QolSavePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolSavePort>::value,
              "QolSavePort must remain trivially copyable");

}  // namespace qol::contracts
