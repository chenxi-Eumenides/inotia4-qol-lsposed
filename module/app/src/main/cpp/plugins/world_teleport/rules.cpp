#include "rules.h"

#include <algorithm>
#include <cstdint>

namespace world_teleport {

int max_map_id_from_record_count(int record_count) {
    if (record_count <= 0) return kMaxMapId;
    return std::min(record_count - 1, kMaxMapId);
}

int target_map_id(int current_id, int delta, int max_map_id) {
    const int upper = max_map_id < 0 ? kMaxMapId : max_map_id;
    if (upper > kMaxMapId || current_id < 0 || current_id > upper) return -1;
    const int64_t result = static_cast<int64_t>(current_id) + static_cast<int64_t>(delta);
    if (result > upper) return 0;
    if (result < 0) return upper;
    return static_cast<int>(result);
}

int target_map_id(int current_id, int delta) {
    return target_map_id(current_id, delta, kMaxMapId);
}

bool pick_landing(const uint8_t* tiles, int width, int height, int* out_x, int* out_y) {
    constexpr int kTileDim = 64;
    constexpr uint8_t kBlockBit = 0x08;
    if (tiles == nullptr || out_x == nullptr || out_y == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    const int w = width < kTileDim ? width : kTileDim;
    const int h = height < kTileDim ? height : kTileDim;
    const auto blocking = [tiles, w, h](int tx, int ty) {
        if (tx < 0 || tx >= w || ty < 0 || ty >= h) return true;
        return (tiles[ty * kTileDim + tx] & kBlockBit) != 0;
    };

    const int cx = w / 2;
    const int cy = h / 2;
    if (!blocking(cx, cy)) {
        *out_x = cx;
        *out_y = cy;
        return true;
    }
    for (int step = 1; step <= 32; ++step) {
        if (!blocking(cx + step, cy)) {
            *out_x = cx + step;
            *out_y = cy;
            return true;
        }
        if (!blocking(cx - step, cy)) {
            *out_x = cx - step;
            *out_y = cy;
            return true;
        }
    }
    for (int step = 1; step <= 32; ++step) {
        if (!blocking(cx, cy + step)) {
            *out_x = cx;
            *out_y = cy + step;
            return true;
        }
        if (!blocking(cx, cy - step)) {
            *out_x = cx;
            *out_y = cy - step;
            return true;
        }
    }
    int best_x = -1;
    int best_y = -1;
    int best_distance = 0;
    for (int ty = 0; ty < h; ++ty) {
        for (int tx = 0; tx < w; ++tx) {
            if (blocking(tx, ty)) continue;
            const int dx = tx > cx ? tx - cx : cx - tx;
            const int dy = ty > cy ? ty - cy : cy - ty;
            const int distance = dx > dy ? dx : dy;
            if (best_x < 0 || distance < best_distance) {
                best_x = tx;
                best_y = ty;
                best_distance = distance;
            }
        }
    }
    if (best_x < 0) return false;
    *out_x = best_x;
    *out_y = best_y;
    return true;
}

}  // namespace world_teleport
