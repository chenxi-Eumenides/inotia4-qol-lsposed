// game_nav.cpp —— 由 game_data.cpp 拆分生成（纯搬代码，零逻辑变更）

#include "game_access.h"
#include "game_symbols.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "game_nav.h"
#include "game_state.h"
#include "game_tiles.h"

const uint8_t* nav_tiles() {
    // P0#瓦片矩阵（2026-08-12）：优先静态数据（assets maps/tiles.json，Kotlin 经 JNI 传入），
    // 缺失时回退运行时读内存 *(*(base+G_TILE_GOT_VMA))。
    const uint8_t* st = static_tiles_for(static_cast<int>(current_map_id()));
    if (st != nullptr) return st;
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<const uint8_t**>(g_base + G_TILE_GOT_VMA);
}

bool nav_blocked(const uint8_t* tiles, int tx, int ty) {
    if (tx < 0 || tx >= NAV_W || ty < 0 || ty >= NAV_H) return true;
    return (tiles[ty * NAV_W + tx] & TILE_BLOCK_BIT) != 0;
}

void nav_unit_blocks(bool* blocks) {
    for (int i = 0; i < NAV_W * NAV_H; ++i) blocks[i] = false;
    if (g_base == 0) return;
    uint8_t* pool = *reinterpret_cast<uint8_t**>(g_base + G_CHAR_POOL_VMA);
    if (pool == nullptr) return;
    void* hero = lead_member();
        for (int i = 0; i < C_CHARSYSTEM_POOL_SLOTS; ++i) {
        uint8_t* obj = pool + i * C_OBJ_SIZE;
        int16_t x = *reinterpret_cast<int16_t*>(obj + C_POS_X);
        int16_t y = *reinterpret_cast<int16_t*>(obj + C_POS_Y);
        int type = static_cast<int>(reinterpret_cast<int8_t*>(obj)[C_TYPE]);
        uint8_t status = obj[C_STATUS];
        // v0.5.28 修复：situation(obj[0])!=1 的对象不参与碰撞（引擎 CHARSYSTEM_GetCharacterBlock 0xddaac
        // 要求 obj[0]==1 才判阻挡）——尸体死亡被 SetSituation(6)/Free(0) 后 situation!=1，但
        // C_STATUS(0x311) 未清零，此前把尸体误判为阻挡（模块 BFS 比引擎保守、绕远/判不可达）。
        if (obj[C_SITUATION] != 1) continue;
        // v0.4.39 修复：type==2（装饰/场景单位，火把/木桶等）也纳入阻挡——
        // CHAR_Move 的 CHARSYSTEM_GetCharacterBlock(F_CHAR_GET_BLOCK_VMA) 把它们当阻挡（ret=2），
        // BFS 若排除 type=2 会规划穿过装饰物的路径 → 走到面前被 ret=2 卡死（真机实测 280,328 卡死）。
        if (type < 0 || type > 2) continue;
        if (status > 2) continue;
        if (x <= 0 || x >= 1500 || y <= 0 || y >= 1500) continue;
        if (hero != nullptr && obj == hero) continue;
        // v0.5.31 修复：按引擎碰撞矩形（CHAR_GetAreaRect）标记覆盖的所有 tile，
        // 对齐 CHARSYSTEM_GetCharacterBlock 的矩形碰撞——碰撞矩形可覆盖相邻格（如
        // 大型装饰物偏移非默认 8,8,8,8），此前只标单格导致假可走格 → 走到面前撞墙。
        if (fn_char_get_area_rect == nullptr) {
            int tx = x >> 4, ty = y >> 4;
            if (tx >= 0 && tx < NAV_W && ty >= 0 && ty < NAV_H) blocks[ty * NAV_W + tx] = true;
            continue;
        }
        int16_t rect[4] = {0, 0, 0, 0};  // [min_x, min_y, max_x, max_y] 绝对像素坐标
        fn_char_get_area_rect(reinterpret_cast<void*>(obj), x, y, rect);
        for (int ty = rect[1] >> 4; ty <= (rect[3] >> 4); ++ty) {
            if (ty < 0 || ty >= NAV_H) continue;
            for (int tx = rect[0] >> 4; tx <= (rect[2] >> 4); ++tx) {
                if (tx >= 0 && tx < NAV_W) blocks[ty * NAV_W + tx] = true;
            }
        }
    }
}

bool nav_bfs(int sx, int sy, int tx, int ty, NavPath& out, bool use_units, int max_steps,
             int block_tx, int block_ty) {
    const uint8_t* tiles = nav_tiles();
    if (tiles == nullptr) return false;
    if (sx < 0 || sx >= NAV_W || sy < 0 || sy >= NAV_H) return false;
    if (nav_blocked(tiles, sx, sy)) {
        for (int d = 0; d < 4; ++d) {
            int nx = sx + NAV_DX[d], ny = sy + NAV_DY[d];
            if (!nav_blocked(tiles, nx, ny)) { sx = nx; sy = ny; break; }
        }
    }
    bool unit_blocks[NAV_W * NAV_H];
    if (use_units) nav_unit_blocks(unit_blocks);
    std::vector<int> prev(NAV_W * NAV_H);
    std::vector<int> depth(NAV_W * NAV_H);
    std::vector<int> queue(NAV_W * NAV_H);
    for (int i = 0; i < NAV_W * NAV_H; ++i) prev[i] = -1;
    auto tidx = [](int x, int y) { return y * NAV_W + x; };
    int head = 0, tail = 0;
    int start = tidx(sx, sy);
    prev[start] = start;
    depth[start] = 0;
    queue[tail++] = start;
    bool target_in = (tx >= 0 && tx < NAV_W && ty >= 0 && ty < NAV_H);
    int target = tidx(tx, ty);
    int best_manhattan = INT32_MAX, best_tile = -1, best_depth = -1;
    int found_target = -1;
    while (head < tail) {
        int cur = queue[head++];
        int cx = cur % NAV_W, cy = cur / NAV_W;
        int dcur = depth[cur];
        if (max_steps > 0 && dcur >= max_steps) continue;
        if (target_in) {
            int manh = (cx > tx ? cx - tx : tx - cx) + (cy > ty ? cy - ty : ty - cy);
            if (manh < best_manhattan) { best_manhattan = manh; best_tile = cur; best_depth = dcur; }
        }
        if (cur == target) { found_target = cur; break; }
        for (int d = 0; d < 4; ++d) {
            int nx = cx + NAV_DX[d], ny = cy + NAV_DY[d];
            if (nx < 0 || nx >= NAV_W || ny < 0 || ny >= NAV_H) continue;
            int ni = tidx(nx, ny);
            if (prev[ni] != -1 || nav_blocked(tiles, nx, ny)) continue;
            if (use_units && unit_blocks[ni]) continue;
            if (block_tx == nx && block_ty == ny) continue;   // 撞墙格临时阻挡（引擎碰撞 vs 模块建模差异）
            prev[ni] = cur;
            depth[ni] = dcur + 1;
            queue[tail++] = ni;
        }
    }
    int end_tile = -1;
    if (found_target != -1) end_tile = found_target;
    else if (best_tile != -1) end_tile = best_tile;
    if (end_tile == -1) return false;
    out.found = (found_target != -1);
    out.distance = depth[end_tile];
    out.nearest_x = best_tile == -1 ? -1 : best_tile % NAV_W;
    out.nearest_y = best_tile == -1 ? -1 : best_tile / NAV_W;
    out.nearest_dist = best_depth;
    {
        int rev[NAV_MAX_DIRS];
        int rev_count = 0;
        int cur = end_tile;
        while (cur != start && rev_count < NAV_MAX_DIRS) {
            int p = prev[cur];
            int dx = (cur % NAV_W) - (p % NAV_W);
            int dy = (cur / NAV_W) - (p / NAV_W);
            int d = -1;
            for (int k = 0; k < 4; ++k) {
                if (NAV_DX[k] == dx && NAV_DY[k] == dy) { d = k; break; }
            }
            if (d < 0) break;
            rev[rev_count++] = d;
            cur = p;
        }
        for (int i = 0; i < rev_count; ++i) out.dirs[rev_count - 1 - i] = rev[i];
        out.dir_count = rev_count;
    }
    return true;
}

bool nav_bfs_multi(int sx, int sy, std::vector<int>& depth_out) {
    const uint8_t* tiles = nav_tiles();
    if (tiles == nullptr) return false;
    if (sx < 0 || sx >= NAV_W || sy < 0 || sy >= NAV_H) return false;
    if (nav_blocked(tiles, sx, sy)) {
        for (int d = 0; d < 4; ++d) {
            int nx = sx + NAV_DX[d], ny = sy + NAV_DY[d];
            if (!nav_blocked(tiles, nx, ny)) { sx = nx; sy = ny; break; }
        }
    }
    bool unit_blocks[NAV_W * NAV_H];
    nav_unit_blocks(unit_blocks);
    depth_out.assign(NAV_W * NAV_H, -1);
    std::vector<int> queue(NAV_W * NAV_H);
    int head = 0, tail = 0;
    int start = sy * NAV_W + sx;
    depth_out[start] = 0;
    queue[tail++] = start;
    while (head < tail) {
        int cur = queue[head++];
        int cx = cur % NAV_W, cy = cur / NAV_W;
        int dcur = depth_out[cur];
        for (int d = 0; d < 4; ++d) {
            int nx = cx + NAV_DX[d], ny = cy + NAV_DY[d];
            if (nx < 0 || nx >= NAV_W || ny < 0 || ny >= NAV_H) continue;
            int ni = ny * NAV_W + nx;
            if (depth_out[ni] != -1 || nav_blocked(tiles, nx, ny)) continue;
            if (unit_blocks[ni]) continue;
            depth_out[ni] = dcur + 1;
            queue[tail++] = ni;
        }
    }
    return true;
}
