#pragma once

#include <cstdint>
#include <vector>

// 导航层：BFS 寻路（基于游戏瓦片矩阵，native 纯算法）。
// 依赖 game_symbols 的 G_TILE_GOT_VMA / G_CHAR_POOL_VMA 与 lead_member（game_state 提供）。

constexpr int NAV_W = 64;
constexpr int NAV_H = 64;
constexpr int NAV_MAX_DIRS = 2048;
static const int NAV_DX[4] = {0, -1, 0, 1};
static const int NAV_DY[4] = {1, 0, -1, 0};

struct NavPath {
    bool found = false;
    int distance = -1;
    int nearest_x = -1, nearest_y = -1, nearest_dist = -1;
    int dirs[NAV_MAX_DIRS];
    int dir_count = 0;
};

const uint8_t* nav_tiles();
bool nav_blocked(const uint8_t* tiles, int tx, int ty);
void nav_unit_blocks(bool* blocks);
// block_tx/block_ty：额外阻挡格（-1 表示无）。撞墙重规划时传"撞墙方向的目标格"，
// 该格模块瓦片/单位建模判可走但引擎 CHAR_Move 判阻挡（碰撞建模差异），临时排除避免重规划又走同格。
bool nav_bfs(int sx, int sy, int tx, int ty, NavPath& out, bool use_units = true, int max_steps = 0,
             int block_tx = -1, int block_ty = -1);
bool nav_bfs_multi(int sx, int sy, std::vector<int>& depth_out);
