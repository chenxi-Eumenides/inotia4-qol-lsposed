#pragma once

#include <climits>
#include <cstdint>
#include <cstring>

#include "game_ports/world_port.h"

namespace qol::contracts {

struct NavBfsResult {
    bool found = false;
    int distance = -1;
    int nearest_x = -1;
    int nearest_y = -1;
    int nearest_distance = -1;
};

// Searches a row-major tile grid; blocked and optional unit_blocks use nonzero for blocked tiles.
// width/height bound the supplied arrays; sx/sy and tx/ty are tile coordinates. A null
// unit_blocks ignores unit occupancy; max_steps <= 0 is unlimited; block_tx/block_ty excludes
// one additional tile, with (-1,-1) meaning no additional exclusion. Result distance is the
// selected route depth; nearest coordinates/depth identify the reachable tile with least
// Manhattan distance to the target, or stay -1 if no candidate exists.
// An out-of-bounds target never matches a visited cell, but still selects the nearest reachable
// tile by Manhattan distance for callers that can recover toward that tile.
// Direction indices are down (0), left (1), up (2), right (3), matching the game's control order.
// A blocked start falls back to the first non-blocked neighbor in that order; unit occupancy and
// the additional exclusion are deliberately ignored for this fallback so it only reflects map
// blocking and preserves the existing start-tile recovery rule. dirs_out may be null to omit the
// route; otherwise dirs_capacity bounds emitted entries and dirs_count receives the emitted count.
inline bool nav_bfs(int width, int height, const uint8_t* blocked, const uint8_t* unit_blocks,
                    int sx, int sy, int tx, int ty, int max_steps, int block_tx, int block_ty,
                    NavBfsResult* result, int8_t* dirs_out = nullptr, int dirs_capacity = 0,
                    int* dirs_count = nullptr) {
    if (result == nullptr) return false;
    *result = NavBfsResult{};
    if (dirs_count != nullptr) *dirs_count = 0;
    if (width <= 0 || height <= 0 || width > static_cast<int>(kWorldNavWidth) ||
        height > static_cast<int>(kWorldNavHeight) || blocked == nullptr || sx < 0 || sx >= width ||
        sy < 0 || sy >= height) {
        return false;
    }

    const int count = width * height;
    int previous[kWorldNavTileCount];
    int depth[kWorldNavTileCount];
    int queue[kWorldNavTileCount];
    int8_t reverse_dirs[kWorldNavTileCount];
    std::memset(previous, 0xff, sizeof(previous));

    constexpr int kDx[] = {0, -1, 0, 1};
    constexpr int kDy[] = {1, 0, -1, 0};
    auto index = [width](int x, int y) { return y * width + x; };
    if (blocked[index(sx, sy)] != 0) {
        for (int d = 0; d < 4; ++d) {
            const int nx = sx + kDx[d];
            const int ny = sy + kDy[d];
            if (nx >= 0 && nx < width && ny >= 0 && ny < height &&
                blocked[index(nx, ny)] == 0) {
                sx = nx;
                sy = ny;
                break;
            }
        }
    }

    const bool target_in = tx >= 0 && tx < width && ty >= 0 && ty < height;
    const int target = target_in ? index(tx, ty) : -1;
    const int start = index(sx, sy);
    previous[start] = start;
    depth[start] = 0;
    int queue_head = 0;
    int queue_tail = 0;
    queue[queue_tail++] = start;
    int64_t best_manhattan = LLONG_MAX;
    int best_tile = -1;
    int best_depth = -1;
    int found_tile = -1;

    while (queue_head < queue_tail) {
        const int current = queue[queue_head++];
        const int cx = current % width;
        const int cy = current / width;
        const int current_depth = depth[current];
        const int64_t delta_x = static_cast<int64_t>(cx) - tx;
        const int64_t delta_y = static_cast<int64_t>(cy) - ty;
        const int64_t manhattan = (delta_x < 0 ? -delta_x : delta_x) +
                                  (delta_y < 0 ? -delta_y : delta_y);
        if (manhattan < best_manhattan) {
            best_manhattan = manhattan;
            best_tile = current;
            best_depth = current_depth;
        }
        if (current == target) {
            found_tile = current;
            break;
        }
        if (max_steps > 0 && current_depth >= max_steps) continue;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + kDx[d];
            const int ny = cy + kDy[d];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const int next = index(nx, ny);
            if (previous[next] != -1 || blocked[next] != 0 ||
                (unit_blocks != nullptr && unit_blocks[next] != 0) ||
                (block_tx == nx && block_ty == ny)) {
                continue;
            }
            previous[next] = current;
            depth[next] = current_depth + 1;
            queue[queue_tail++] = next;
        }
    }

    const int end_tile = found_tile != -1 ? found_tile : best_tile;
    if (end_tile == -1) return false;
    result->found = found_tile != -1;
    result->distance = depth[end_tile];
    if (best_tile != -1) {
        result->nearest_x = best_tile % width;
        result->nearest_y = best_tile / width;
        result->nearest_distance = best_depth;
    }

    int reverse_count = 0;
    for (int current = end_tile; current != start && reverse_count < count;
         current = previous[current]) {
        const int parent = previous[current];
        if (parent < 0) break;
        const int dx = current % width - parent % width;
        const int dy = current / width - parent / width;
        int direction = -1;
        for (int d = 0; d < 4; ++d) {
            if (kDx[d] == dx && kDy[d] == dy) {
                direction = d;
                break;
            }
        }
        if (direction < 0) break;
        reverse_dirs[reverse_count++] = static_cast<int8_t>(direction);
    }
    const int emitted = dirs_out == nullptr || dirs_capacity <= 0
                            ? 0
                            : (reverse_count < dirs_capacity ? reverse_count : dirs_capacity);
    for (int i = 0; i < emitted; ++i) dirs_out[i] = reverse_dirs[reverse_count - 1 - i];
    if (dirs_count != nullptr) *dirs_count = emitted;
    return true;
}

}  // namespace qol::contracts
