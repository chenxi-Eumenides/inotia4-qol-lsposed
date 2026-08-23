// host 纯函数单测（G4 裁决）：
// 覆盖 json_escape / base64_decode / parse_int_field / nav_bfs / nav_bfs_multi / tiles 解析。
// 全部编译真实被测源文件（非复制实现）：
//   - game_json.cpp   → json_escape（纯 STL）
//   - game_nav.cpp    → nav_bfs / nav_bfs_multi（BFS 纯算法，g_base=0 时 nav_unit_blocks 退化为空）
//   - game_tiles.cpp  → 本文件直接 #include，以访问匿名命名空间的 base64_decode/parse_int_field，
//                       并复用 set_static_tiles 注入瓦片矩阵
// 被测代码依赖的 Android 头由 stubs/android/log.h 覆盖；游戏内存符号由 test_stubs.cpp 提供。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game_json.h"
#include "game_nav.h"
#include "game_tiles.h"
#include "stack_codec.h"
#include "virtual_bag_state.h"
#include "../game_tiles.cpp"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) {                                                        \
            ++g_pass;                                                      \
        } else {                                                           \
            ++g_fail;                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto va = (a);                                                     \
        auto vb = (b);                                                     \
        if (va == vb) {                                                    \
            ++g_pass;                                                      \
        } else {                                                           \
            ++g_fail;                                                      \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                  \
    } while (0)

static std::string base64_encode(const uint8_t* data, size_t n) {
    // 标准 RFC4648 big-endian 编码（与 Python 生成 tiles.json 一致，可被被测 base64_decode 正确回解）。
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += b64[(v >> 18) & 0x3F];
        out += b64[(v >> 12) & 0x3F];
        out += b64[(v >> 6) & 0x3F];
        out += b64[v & 0x3F];
    }
    if (i + 1 == n) {
        uint32_t v = uint32_t(data[i]) << 16;
        out += b64[(v >> 18) & 0x3F];
        out += b64[(v >> 12) & 0x3F];
        out += '=';
        out += '=';
    } else if (i + 2 == n) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += b64[(v >> 18) & 0x3F];
        out += b64[(v >> 12) & 0x3F];
        out += b64[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

static std::string make_map_json(int map_id, const uint8_t* tiles, int w, int h) {
    std::string b64 = base64_encode(tiles, STATIC_TILE_BYTES);
    return "{\"m" + std::to_string(map_id) +
           "\":{\"mapId\":" + std::to_string(map_id) +
           ",\"width\":" + std::to_string(w) +
           ",\"height\":" + std::to_string(h) +
           ",\"tiles\":\"" + b64 + "\"}}";
}

static void test_json_escape() {
    CHECK_EQ(json_escape("plain"), "plain");
    CHECK_EQ(json_escape(""), "");
    CHECK_EQ(json_escape(nullptr), "");
    CHECK_EQ(json_escape("a\"b"), "a\\\"b");
    CHECK_EQ(json_escape("a\\b"), "a\\\\b");
    CHECK_EQ(json_escape("a\nb"), "a\\nb");
    CHECK_EQ(json_escape("a\rb"), "a\\rb");
    CHECK_EQ(json_escape("a\tb"), "a\\tb");
    CHECK_EQ(json_escape("a\x01" "b"), "a\\u0001b");
    CHECK_EQ(json_escape("中文"), "中文");  // 高位字节原样保留
}

static void test_base64_decode() {
    uint8_t out[5] = {0};
    CHECK(base64_decode("SGVsbG8=", out, 5));           // "Hello"
    CHECK_EQ(std::string(reinterpret_cast<char*>(out), 5), "Hello");

    uint8_t out2[4] = {0};
    CHECK(!base64_decode("SGVsbG8=", out2, 4));         // 5B 解码但容量 4 → 失败

    uint8_t out3[4] = {0};
    CHECK(!base64_decode("!!!", out3, 4));              // 非法字符 → 失败

    uint8_t out4[4] = {0};
    CHECK(base64_decode("AAABAg==", out4, 4));          // {0,0,1,2}
    CHECK_EQ((int)out4[0], 0);
    CHECK_EQ((int)out4[1], 0);
    CHECK_EQ((int)out4[2], 1);
    CHECK_EQ((int)out4[3], 2);
}

static void test_parse_int_field() {
    std::string j = "{\"width\":64,\"height\":48,\"n\":-5}";
    CHECK_EQ(parse_int_field(j, 0, "width"), 64);
    CHECK_EQ(parse_int_field(j, 0, "height"), 48);
    CHECK_EQ(parse_int_field(j, 0, "missing"), 0);
}

static void test_tiles_parse() {
    uint8_t tiles[STATIC_TILE_BYTES];
    std::memset(tiles, 0, sizeof(tiles));
    tiles[0] = 0x08;  // (0,0) 阻挡
    tiles[1] = 0x80;  // (1,0) 出口
    set_static_tiles(make_map_json(7, tiles, 64, 64));

    CHECK(static_tiles_ready());
    const uint8_t* got = static_tiles_for(7);
    CHECK(got != nullptr);
    CHECK_EQ((int)got[0], 0x08);
    CHECK_EQ((int)got[1], 0x80);
    CHECK_EQ((int)got[2], 0x00);
    CHECK_EQ(static_tiles_width(7), 64);
    CHECK_EQ(static_tiles_height(7), 64);
    CHECK(static_tiles_for(999) == nullptr);
}

static void test_nav_bfs() {
    uint8_t tiles[STATIC_TILE_BYTES];

    // 空地：曼哈顿直连
    std::memset(tiles, 0, sizeof(tiles));
    set_static_tiles(make_map_json(0, tiles, 64, 64));
    NavPath np;
    CHECK(nav_bfs(0, 0, 5, 5, np));
    CHECK(np.found);
    CHECK_EQ(np.distance, 10);
    CHECK(np.dir_count > 0);
    CHECK_EQ(np.dir_count, 10);

    // 顶部墙 x=1..4 @ y=0：从 (0,0) 到 (5,0) 须下绕 → 距离 7
    std::memset(tiles, 0, sizeof(tiles));
    for (int x = 1; x <= 4; ++x) tiles[x] = 0x08;
    set_static_tiles(make_map_json(0, tiles, 64, 64));
    NavPath np2;
    CHECK(nav_bfs(0, 0, 5, 0, np2));
    CHECK(np2.found);
    CHECK_EQ(np2.distance, 7);

    // 目标被四面墙包围 → found=false，返回 nearest
    std::memset(tiles, 0, sizeof(tiles));
    tiles[4 * 64 + 5] = 0x08;
    tiles[6 * 64 + 5] = 0x08;
    tiles[5 * 64 + 4] = 0x08;
    tiles[5 * 64 + 6] = 0x08;
    set_static_tiles(make_map_json(0, tiles, 64, 64));
    NavPath np3;
    CHECK(nav_bfs(0, 0, 5, 5, np3));
    CHECK(!np3.found);
    CHECK(np3.nearest_x >= 0);

    // nav_blocked
    const uint8_t* t = static_tiles_for(0);
    CHECK(t != nullptr);
    CHECK(nav_blocked(t, 5, 4));
    CHECK(!nav_blocked(t, 5, 5));
    CHECK(nav_blocked(t, -1, 0));   // 越界视为阻挡
    CHECK(nav_blocked(t, 64, 0));
}

static void test_nav_bfs_multi() {
    uint8_t tiles[STATIC_TILE_BYTES];

    std::memset(tiles, 0, sizeof(tiles));
    set_static_tiles(make_map_json(0, tiles, 64, 64));
    std::vector<int> depth;
    CHECK(nav_bfs_multi(0, 0, depth));
    CHECK_EQ(depth.size(), static_cast<size_t>(NAV_W * NAV_H));
    CHECK_EQ(depth[0], 0);
    CHECK_EQ(depth[1], 1);
    CHECK_EQ(depth[64], 1);
    CHECK_EQ(depth[5 * 64 + 5], 10);

    // 除起点外全阻挡：仅起点可达
    std::memset(tiles, 0, sizeof(tiles));
    for (int i = 0; i < STATIC_TILE_BYTES; ++i) tiles[i] = 0x08;
    tiles[0] = 0;
    set_static_tiles(make_map_json(0, tiles, 64, 64));
    std::vector<int> d2;
    CHECK(nav_bfs_multi(0, 0, d2));
    CHECK_EQ(d2[0], 0);
    CHECK_EQ(d2[1], -1);
    CHECK_EQ(d2[64], -1);
}

static void test_stack_codec() {
    const uint32_t low = 0x0003A55Fu;
    for (uint32_t count : {0u, 1u, 99u, 100u, 127u, 128u, 999u}) {
        uint32_t encoded = stack_codec::write_count(low, count);
        CHECK_EQ(stack_codec::read_count(encoded), count);
        CHECK_EQ(encoded & ~stack_codec::kCountMask, low);
    }
    CHECK_EQ(stack_codec::clamp_count(1000, true), 999u);
    CHECK_EQ(stack_codec::clamp_count(1000, false), 99u);
    CHECK_EQ(stack_codec::clamp_count(99, false), 99u);
    CHECK_EQ(stack_codec::write_count(low, 1000), low | (1000u << 22));
    CHECK_EQ(stack_codec::write_count(low, 999) & stack_codec::kCountMask, 999u << 22);
}

static void test_virtual_bag_state() {
    virtual_bag::State state{};
    state.capacities = {0, 0, 0, 0, 0};
    state.types = {4, 4, 4, 4, 4};
    virtual_bag::normalize(&state);
    CHECK_EQ((int)state.capacities[0], 16);
    CHECK_EQ((int)state.capacities[1], 8);
    CHECK_EQ((int)state.capacities[2], 4);
    CHECK_EQ((int)state.capacities[3], 0);
    CHECK_EQ((int)state.capacities[4], 0);
    state.mode = virtual_bag::Mode::kModule;
    state.selected = 3;
    virtual_bag::normalize(&state);
    CHECK_EQ(state.mode, virtual_bag::Mode::kOriginal);
    CHECK_EQ(state.selected, -1);
    CHECK_EQ((int)state.capacities[0], 16);
    CHECK_EQ((int)state.capacities[1], 8);
    CHECK_EQ((int)state.capacities[2], 4);
    CHECK_EQ((int)state.capacities[3], 0);
    CHECK_EQ((int)state.capacities[4], 0);
    CHECK(virtual_bag::set_test_equipped(&state, 0, 1));
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.mode, virtual_bag::Mode::kModule);
    CHECK_EQ(state.selected, 0);
    CHECK_EQ(state.inspected, -1);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kInspected);
    CHECK_EQ(state.inspected, 0);
    state = {};
    CHECK(virtual_bag::set_test_equipped(&state, 0, 1));
    CHECK_EQ((int)state.types[0], 1);
    CHECK_EQ((int)state.capacities[0], 16);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.selected, 0);
    CHECK_EQ(state.inspected, -1);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kInspected);
    CHECK_EQ(state.inspected, 0);
    CHECK(virtual_bag::set_test_equipped(&state, 1, 4));
    CHECK_EQ((int)state.capacities[1], 8);
    CHECK_EQ(virtual_bag::click(&state, 1), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.selected, 1);
    CHECK_EQ(state.inspected, -1);
    CHECK(!virtual_bag::set_test_equipped(&state, 5, 1));
    CHECK(!virtual_bag::set_test_equipped(&state, 2, 5));
    CHECK_EQ(virtual_bag::click(&state, 3), virtual_bag::ClickResult::kIgnored);
    CHECK_EQ(virtual_bag::click(&state, 2), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.selected, 2);
    CHECK(virtual_bag::set_item(&state, 2, 3, 401, 7));
    CHECK_EQ(state.items[2][3].category, 401);
    CHECK_EQ(state.items[2][3].count, 7);
    virtual_bag::begin_exit_module(&state);
    CHECK_EQ(state.mode, virtual_bag::Mode::kExitingModule);
    CHECK_EQ(state.selected, 2);
    CHECK_EQ(state.inspected, -1);
    virtual_bag::enter_original(&state, 4);
    CHECK_EQ(state.mode, virtual_bag::Mode::kOriginal);
    CHECK_EQ(state.original_selected, 4);
    CHECK_EQ(state.selected, -1);
    CHECK_EQ(state.inspected, -1);
}

static void test_extension_bag_exit_rendering_state() {
    virtual_bag::State state{};
    CHECK(virtual_bag::set_test_equipped(&state, 0, 1));
    CHECK(virtual_bag::set_item(&state, 0, 0, 401, 7));
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kSelected);

    virtual_bag::begin_exit_module(&state);
    CHECK_EQ(state.mode, virtual_bag::Mode::kExitingModule);
    CHECK(state.mode != virtual_bag::Mode::kModule);
    CHECK(!(state.mode == virtual_bag::Mode::kModule &&
            virtual_bag::valid_index(state.selected)));
    CHECK_EQ(state.selected, 0);
    CHECK_EQ(state.inspected, -1);
    CHECK_EQ((int)state.capacities[0], 16);
    CHECK_EQ(state.items[0][0].category, 401);
    CHECK_EQ(state.items[0][0].count, 7);

    virtual_bag::normalize(&state);
    CHECK_EQ(state.mode, virtual_bag::Mode::kExitingModule);
    CHECK_EQ(state.selected, 0);

    virtual_bag::enter_original(&state, 3);
    CHECK_EQ(state.mode, virtual_bag::Mode::kOriginal);
    CHECK_EQ(state.original_selected, 3);
    CHECK_EQ(state.selected, -1);
    CHECK_EQ(state.inspected, -1);

    virtual_bag::enter_original(&state, 6);
    CHECK_EQ(state.original_selected, 0);
}

int main() {
    test_json_escape();
    test_base64_decode();
    test_parse_int_field();
    test_tiles_parse();
    test_nav_bfs();
    test_nav_bfs_multi();
    test_stack_codec();
    test_virtual_bag_state();
    test_extension_bag_exit_rendering_state();

    std::printf("host_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
