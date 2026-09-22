#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

constexpr uint32_t kWorldNavWidth = 64;
constexpr uint32_t kWorldNavHeight = 64;
constexpr uint32_t kWorldNavTileCount = kWorldNavWidth * kWorldNavHeight;

struct QolWorldNavigation {
    int32_t player_x;
    int32_t player_y;
    uint8_t valid;
    uint8_t blocked[kWorldNavTileCount];
    uint8_t unit_blocks[kWorldNavTileCount];
};

using QolWorldJsonFn = QolResult (*)(char* out, uint32_t capacity, uint32_t* out_size);
using QolWorldNavigationFn = QolResult (*)(QolWorldNavigation* out_navigation);

struct QolWorldPortReadApi {
    QolResult (*state)(int32_t* out_state);
    QolResult (*game_state_gate)(int32_t* out_open);
    QolResult (*map_record_count)(int32_t* out_count);
    QolResult (*current_map_id)(int32_t* out_map_id);
    QolResult (*map_name)(int32_t map_id, const char** out_name);
    QolResult (*active_player_direction)(int32_t* out_direction);
    QolResult (*money)(int64_t* out_money);
    QolResult (*tutorial_state)(int32_t* out_state);
    QolWorldJsonFn exits_json;
    QolWorldJsonFn units_json;
    QolWorldJsonFn enemies_json;
    QolWorldJsonFn interactives_json;
    QolWorldJsonFn drops_json;
    QolWorldNavigationFn navigation;
    // 是否处于 world 态（非 world 时快照发布 not in game 错误体，与迁移前口径一致）。
    QolResult (*in_world)(int32_t* out_in_world);
};

struct QolWorldPort {
    QolVersion abi_version;
    QolWorldPortReadApi read;
};

}  // namespace qol::contracts
