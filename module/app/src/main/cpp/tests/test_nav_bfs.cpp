#include "contracts/nav_bfs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (false)

constexpr int kWidth = 5;
constexpr int kHeight = 5;
constexpr int kCount = kWidth * kHeight;

int index(int x, int y) { return y * kWidth + x; }

void test_shortest_path_and_directions() {
    uint8_t blocked[kCount]{};
    int8_t dirs[16]{};
    int dir_count = -1;
    qol::contracts::NavBfsResult result{};
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, nullptr, 0, 0, 0, 3, 0, -1, -1,
                                  &result, dirs, 16, &dir_count));
    CHECK(result.found && result.distance == 3);
    CHECK(dir_count == 3 && dirs[0] == 0 && dirs[1] == 0 && dirs[2] == 0);
    int x = 0;
    int y = 0;
    for (int i = 0; i < dir_count; ++i) {
        static constexpr int dx[] = {0, -1, 0, 1};
        static constexpr int dy[] = {1, 0, -1, 0};
        CHECK(dirs[i] >= 0 && dirs[i] <= 3);
        x += dx[dirs[i]];
        y += dy[dirs[i]];
        CHECK(x >= 0 && x < kWidth && y >= 0 && y < kHeight);
        CHECK(blocked[index(x, y)] == 0);
    }
    CHECK(x == 0 && y == 3);
}

void test_obstacle_and_block_tile() {
    uint8_t blocked[kCount]{};
    blocked[index(1, 0)] = 1;
    int8_t dirs[16]{};
    int dir_count = 0;
    qol::contracts::NavBfsResult result{};
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, nullptr, 0, 0, 2, 0, 0, -1, -1,
                                  &result, dirs, 16, &dir_count));
    CHECK(result.found && result.distance == 4);

    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, nullptr, 0, 0, 1, 0, 0, 1, 0,
                                  &result));
    CHECK(!result.found);
    CHECK(!(result.nearest_x == 1 && result.nearest_y == 0));
}

void test_blocked_start_fallback() {
    uint8_t blocked[kCount]{};
    blocked[index(2, 2)] = 1;
    uint8_t units[kCount]{};
    units[index(2, 3)] = 1;
    qol::contracts::NavBfsResult result{};
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, units, 2, 2, 2, 4, 0, -1, -1,
                                  &result));
    CHECK(result.found && result.distance == 1);
}

void test_out_of_bounds_and_unreachable_nearest() {
    uint8_t blocked[kCount]{};
    qol::contracts::NavBfsResult result{};
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, nullptr, 1, 1, kWidth, 0, 0, -1,
                                  -1, &result));
    CHECK(!result.found && result.distance == 4);
    CHECK(result.nearest_x == 4 && result.nearest_y == 0 && result.nearest_distance == 4);

    std::memset(blocked, 0, sizeof(blocked));
    for (int y = 0; y < kHeight; ++y) blocked[index(2, y)] = 1;
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, nullptr, 0, 2, 4, 2, 0, -1, -1,
                                  &result));
    CHECK(!result.found && result.nearest_x == 1 && result.nearest_y == 2);
    CHECK(result.nearest_distance == 1 && result.distance == 1);
}

void test_step_limit_and_units() {
    uint8_t blocked[kCount]{};
    uint8_t units[kCount]{};
    qol::contracts::NavBfsResult result{};
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, nullptr, 0, 0, 0, 3, 2, -1, -1,
                                  &result));
    CHECK(!result.found && result.distance == 2);

    units[index(0, 1)] = 1;
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, units, 0, 0, 0, 1, 0, -1, -1,
                                  &result));
    CHECK(!result.found);
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, nullptr, 0, 0, 0, 1, 0, -1, -1,
                                  &result));
    CHECK(result.found);
    std::memset(blocked, 1, sizeof(blocked));
    CHECK(qol::contracts::nav_bfs(kWidth, kHeight, blocked, nullptr, 2, 2, 3, 2, 0, -1, -1,
                                  &result));
    CHECK(!result.found && result.nearest_x == 2 && result.nearest_y == 2);
}

}  // namespace

int main() {
    test_shortest_path_and_directions();
    test_obstacle_and_block_tile();
    test_blocked_start_fallback();
    test_out_of_bounds_and_unreachable_nearest();
    test_step_limit_and_units();
    std::printf("nav_bfs: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
