#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolWorldPortReadApi {
    QolResult (*state)(int32_t* out_state);
    QolResult (*game_state_gate)(int32_t* out_open);
    QolResult (*map_record_count)(int32_t* out_count);
    QolResult (*current_map_id)(int32_t* out_map_id);
    QolResult (*map_name)(int32_t map_id, const char** out_name);
    QolResult (*active_player_direction)(int32_t* out_direction);
    QolResult (*money)(int64_t* out_money);
    QolResult (*tutorial_state)(int32_t* out_state);
};

struct QolWorldPort {
    QolVersion abi_version;
    QolWorldPortReadApi read;
};

}  // namespace qol::contracts
