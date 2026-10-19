#pragma once

#include <cstdint>

#include "result.h"
#include "version.h"

namespace qol::contracts {

// 静态地图数据分属两个静态源：
// - 逻辑宽高与瓦片通行矩阵来自 maps/tiles.json（每 map 一项，瓦片固定 64×64 字节）；
// - 传送点/出口来自 maps/exits.json，不在瓦片矩阵内（见 QolStaticMapExit）。
constexpr const char* kStaticDataCapabilityId = "static.data";
constexpr QolVersion kStaticDataVersion{1, 1};

struct QolStaticMapExit {
    int32_t x;
    int32_t y;
    int32_t target_map_id;
    int32_t target_x;
    int32_t target_y;
};

// 瓦片矩阵契约（与 maps/tiles.json 同口径）：64×64 通行字节，bit3=不可通行。
constexpr int32_t kStaticMapTileDim = 64;
constexpr uint32_t kStaticMapTileSize =
    static_cast<uint32_t>(kStaticMapTileDim * kStaticMapTileDim);  // 4096

struct QolStaticMapInfo {
    int32_t map_id;
    int32_t width;   // 逻辑地图宽（瓦片列数）
    int32_t height;  // 逻辑地图高（瓦片行数）
};

struct QolStaticDataApi {
    QolResult (*map_name)(int32_t map_id, char* out, uint32_t capacity, uint32_t* out_size);
    QolResult (*map_exits)(int32_t map_id, QolStaticMapExit* out, uint32_t capacity,
                           uint32_t* out_count);
    QolResult (*map_info)(int32_t map_id, QolStaticMapInfo* out);
    QolResult (*map_tiles)(int32_t map_id, uint8_t* out_tiles, uint32_t capacity,
                           uint32_t* out_size);
};

}  // namespace qol::contracts
