#include "game/adapter_ports/world_action_port.h"

#include <cstdlib>
#include <cstring>

#include "contracts/game_ports/ports.h"
#include "game/read/game_tiles.h"
#include "game/symbols/game_symbols.h"

namespace qol::game {
namespace {

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult landing_not_ready() {
    return error_result(kQolErrNotReady, "static tiles unavailable");
}

// 传送落点选点（v1.1）：中点锚；阻挡则先 X 正/负、再 Y 正/负（步长 ≤32，c18 进图
// 选点同口径），仍阻挡做全图最近非阻挡格兜底。无该图静态数据返回错误，调用方
// 回退 (0,0) 字面落点（各改版进图补丁对 (0,0) 有自己的寻位语义）。
QolResult pick_landing(int32_t map_id, int32_t* out_x, int32_t* out_y) {
    if (out_x == nullptr || out_y == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!static_tiles_ready()) return landing_not_ready();
    const uint8_t* tiles = static_tiles_for(map_id);
    const int w = static_tiles_width(map_id);
    const int h = static_tiles_height(map_id);
    if (tiles == nullptr || w <= 0 || h <= 0) return landing_not_ready();
    const int bw = w < STATIC_TILE_W ? w : STATIC_TILE_W;
    const int bh = h < STATIC_TILE_H ? h : STATIC_TILE_H;
    const auto blocking = [tiles, bw, bh](int tx, int ty) {
        if (tx < 0 || tx >= bw || ty < 0 || ty >= bh) return true;
        return (tiles[ty * STATIC_TILE_W + tx] & TILE_BLOCK_BIT) != 0;
    };

    int cx = bw / 2;
    int cy = bh / 2;
    if (!blocking(cx, cy)) {
        *out_x = cx;
        *out_y = cy;
        return contracts::ok();
    }
    for (int step = 1; step <= 32; ++step) {
        if (!blocking(cx + step, cy)) {
            *out_x = cx + step;
            *out_y = cy;
            return contracts::ok();
        }
        if (!blocking(cx - step, cy)) {
            *out_x = cx - step;
            *out_y = cy;
            return contracts::ok();
        }
    }
    for (int step = 1; step <= 32; ++step) {
        if (!blocking(cx, cy + step)) {
            *out_x = cx;
            *out_y = cy + step;
            return contracts::ok();
        }
        if (!blocking(cx, cy - step)) {
            *out_x = cx;
            *out_y = cy - step;
            return contracts::ok();
        }
    }
    int best_x = -1;
    int best_y = -1;
    int best_dist = 0;
    for (int ty = 0; ty < bh; ++ty) {
        for (int tx = 0; tx < bw; ++tx) {
            if (blocking(tx, ty)) continue;
            const int dx = tx > cx ? tx - cx : cx - tx;
            const int dy = ty > cy ? ty - cy : cy - ty;
            const int dist = dx > dy ? dx : dy;
            if (best_x < 0 || dist < best_dist) {
                best_x = tx;
                best_y = ty;
                best_dist = dist;
            }
        }
    }
    if (best_x < 0) return landing_not_ready();
    *out_x = best_x;
    *out_y = best_y;
    return contracts::ok();
}

}  // namespace

const contracts::QolWorldActionPort& world_action_port() {
    static const contracts::QolWorldActionPort port{
        contracts::kQolPortWorldActionVersion, game_world_action_api(), &pick_landing};
    return port;
}

}  // namespace qol::game
