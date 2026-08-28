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
#include "game_save_preflight.h"
#include "game_tiles.h"
#include "ownership_ledger.h"
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
    CHECK_EQ((int)virtual_bag::derive_capacity(0), 0);
    CHECK_EQ((int)virtual_bag::derive_capacity(1), 4);
    CHECK_EQ((int)virtual_bag::derive_capacity(2), 8);
    CHECK_EQ((int)virtual_bag::derive_capacity(3), 12);
    CHECK_EQ((int)virtual_bag::derive_capacity(4), 16);
    CHECK_EQ((int)virtual_bag::derive_capacity(-1), 0);
    CHECK_EQ((int)virtual_bag::derive_capacity(5), 0);
    virtual_bag::State state{};
    virtual_bag::normalize(&state);
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        CHECK_EQ((int)state.capacities[index], 0);
    }
    state.types = {4, 4, 4, 4, 4};
    virtual_bag::normalize(&state);
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        CHECK_EQ((int)state.capacities[index], 16);
    }
    state.mode = virtual_bag::Mode::kModule;
    state.selected = 3;
    virtual_bag::normalize(&state);
    CHECK_EQ(state.mode, virtual_bag::Mode::kModule);
    CHECK_EQ(state.selected, 3);
    state.types[3] = 0;
    virtual_bag::normalize(&state);
    CHECK_EQ(state.mode, virtual_bag::Mode::kOriginal);
    CHECK_EQ(state.selected, -1);
    CHECK_EQ((int)state.capacities[3], 0);
    state.items[3][5] = {};
    state.items[3][5].category = 7;
    state.items[3][5].count = 3;
    virtual_bag::normalize(&state);
    CHECK_EQ(state.items[3][5].category, 7);
    CHECK_EQ(state.items[3][5].count, 3);
    CHECK(virtual_bag::set_test_equipped(&state, 0, 1));
    CHECK_EQ((int)state.capacities[0], 4);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.mode, virtual_bag::Mode::kModule);
    CHECK_EQ(state.selected, 0);
    CHECK_EQ(state.inspected, -1);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kInspected);
    CHECK_EQ(state.inspected, 0);
    state = {};
    CHECK(virtual_bag::set_test_equipped(&state, 0, 1));
    CHECK_EQ((int)state.types[0], 1);
    CHECK_EQ((int)state.capacities[0], 4);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.selected, 0);
    CHECK_EQ(state.inspected, -1);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kInspected);
    CHECK_EQ(state.inspected, 0);
    CHECK(virtual_bag::set_test_equipped(&state, 1, 4));
    CHECK_EQ((int)state.capacities[1], 16);
    CHECK_EQ(virtual_bag::click(&state, 1), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.selected, 1);
    CHECK_EQ(state.inspected, -1);
    CHECK(!virtual_bag::set_test_equipped(&state, 5, 1));
    CHECK(!virtual_bag::set_test_equipped(&state, 2, 5));
    CHECK_EQ(virtual_bag::click(&state, 3), virtual_bag::ClickResult::kIgnored);
    CHECK_EQ(virtual_bag::click(&state, 2), virtual_bag::ClickResult::kIgnored);
    CHECK(virtual_bag::set_test_equipped(&state, 2, 3));
    CHECK_EQ((int)state.capacities[2], 12);
    CHECK_EQ(virtual_bag::click(&state, 2), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.selected, 2);
    CHECK(virtual_bag::set_item(&state, 2, 3, 401, 7));
    CHECK_EQ(state.items[2][3].category, 401);
    CHECK_EQ(state.items[2][3].count, 7);
    state.items[2][4].category = 2;
    state.items[2][4].count = 1;
    state.items[2][4].payload_size = virtual_bag::kPayloadHeaderSize;
    state.items[2][4].payload[0] = 0;
    virtual_bag::normalize(&state);
    CHECK_EQ(state.items[2][4].category, 0);
    CHECK_EQ(state.items[2][4].count, 0);
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
    CHECK_EQ((int)state.capacities[0], 4);
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

static void make_small_payload(virtual_bag::Item* item, uint32_t count) {
    item->payload_size = 19;
    item->payload[0] = static_cast<uint8_t>(item->payload_size - 1);
    const uint32_t count_u32 = stack_codec::write_count(0x00012345u, count);
    for (size_t i = 0; i < 4; ++i) {
        item->payload[virtual_bag::kPayloadCountOffset + i] =
            static_cast<uint8_t>((count_u32 >> (8 * i)) & 0xFF);
    }
}

static void test_virtual_bag_base64() {
    uint8_t data[255];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i * 7 + 1);
    for (size_t len : {1u, 2u, 3u, 4u, 5u, 19u, 255u}) {
        const std::string enc = virtual_bag::base64_encode(data, len);
        uint8_t dec[256] = {0};
        const int n = virtual_bag::base64_decode(enc.c_str(), enc.size(), dec, sizeof(dec));
        CHECK_EQ(n, static_cast<int>(len));
        CHECK(std::memcmp(dec, data, len) == 0);
    }
    uint8_t out[256] = {0};
    CHECK_EQ(virtual_bag::base64_decode("!!!!", 4, out, sizeof(out)), 0);
    CHECK_EQ(virtual_bag::base64_decode("A", 1, out, sizeof(out)), 0);
    CHECK_EQ(virtual_bag::base64_decode("AA=A", 4, out, sizeof(out)), 0);
    uint8_t small[2] = {0};
    CHECK_EQ(virtual_bag::base64_decode("AAAA", 4, small, 2), 0);
    CHECK_EQ(virtual_bag::base64_decode("", 0, out, sizeof(out)), 0);
}

static void test_virtual_bag_payload_helpers() {
    virtual_bag::Item item{};
    item.category = 401;
    item.count = 3;
    make_small_payload(&item, 3);
    CHECK(virtual_bag::valid_payload(item));
    CHECK_EQ(static_cast<int>(stack_codec::read_count(virtual_bag::payload_count(item))), 3);
    virtual_bag::patch_payload_count(&item, 7);
    CHECK_EQ(static_cast<int>(stack_codec::read_count(virtual_bag::payload_count(item))), 7);
    CHECK_EQ(virtual_bag::payload_count(item) & ~stack_codec::kCountMask, 0x00012345u);
    virtual_bag::patch_payload_count(&item, 999);
    CHECK_EQ(static_cast<int>(stack_codec::read_count(virtual_bag::payload_count(item))), 999);
    const uint32_t hash = virtual_bag::payload_hash(item);
    item.count = 1;
    CHECK(virtual_bag::payload_hash(item) != hash);
}

static void test_virtual_bag_merge_count() {
    CHECK_EQ(virtual_bag::merge_count(3, 5, false), 8u);
    CHECK_EQ(virtual_bag::merge_count(95, 10, false), 99u);
    CHECK_EQ(virtual_bag::merge_count(95, 10, true), 105u);
    CHECK_EQ(virtual_bag::merge_count(990, 20, true), 999u);
    CHECK_EQ(virtual_bag::merge_count(0, 0, false), 0u);
}

static void test_virtual_bag_mergeable_items() {
    virtual_bag::Item existing{};
    virtual_bag::Item source{};
    existing.category = 401;
    source.category = 401;
    existing.count = 3;
    source.count = 5;
    make_small_payload(&existing, existing.count);
    make_small_payload(&source, source.count);
    CHECK(virtual_bag::mergeable_items(existing, source));
    source.category = 402;
    CHECK(!virtual_bag::mergeable_items(existing, source));
    source.category = existing.category;
    source.payload[7] ^= 1;
    CHECK(!virtual_bag::mergeable_items(existing, source));
    source = virtual_bag::Item{existing.category, source.count};
    virtual_bag::Item legacy_existing{existing.category, existing.count};
    CHECK(virtual_bag::mergeable_items(legacy_existing, source));
}

static void test_virtual_bag_json_roundtrip() {
    virtual_bag::State state{};
    state.types = {4, 4, 4, 4, 4};
    virtual_bag::normalize(&state);
    virtual_bag::Item payload_item{};
    payload_item.category = 401;
    payload_item.count = 7;
    make_small_payload(&payload_item, 7);
    state.items[1][2] = payload_item;
    virtual_bag::set_item(&state, 2, 3, 55, 9);
    state.pending.valid = true;
    state.pending.direction = virtual_bag::kTransferOriginalToExtension;
    state.pending.src_bag = 0;
    state.pending.src_slot = 4;
    state.pending.dst_bag = 1;
    state.pending.dst_slot = 2;
    state.pending.payload_size = payload_item.payload_size;
    state.pending.payload = payload_item.payload;
    state.pending.source_payload_size = payload_item.payload_size;
    state.pending.source_payload = payload_item.payload;

    const std::string json = virtual_bag::state_json(state);
    virtual_bag::State parsed{};
    CHECK(virtual_bag::parse_state_json(json.c_str(), &parsed));
    CHECK_EQ(parsed.items[1][2].category, 401);
    CHECK_EQ(parsed.items[1][2].count, 7);
    CHECK_EQ(static_cast<int>(parsed.items[1][2].payload_size), 19);
    CHECK(std::memcmp(parsed.items[1][2].payload.data(), payload_item.payload.data(), 19) == 0);
    CHECK_EQ(parsed.items[2][3].category, 55);
    CHECK_EQ(parsed.items[2][3].count, 9);
    CHECK_EQ(static_cast<int>(parsed.items[2][3].payload_size), 0);
    CHECK(parsed.pending.valid);
    CHECK_EQ(static_cast<int>(parsed.pending.direction),
             static_cast<int>(virtual_bag::kTransferOriginalToExtension));
    CHECK_EQ(static_cast<int>(parsed.pending.dst_slot), 2);
    CHECK_EQ(static_cast<int>(parsed.pending.payload_size), 19);
    CHECK(std::memcmp(parsed.pending.payload.data(), payload_item.payload.data(), 19) == 0);
    CHECK_EQ(static_cast<int>(parsed.pending.source_payload_size), 19);
    CHECK(std::memcmp(parsed.pending.source_payload.data(), payload_item.payload.data(), 19) == 0);

    const std::string encoded = virtual_bag::base64_encode(
        payload_item.payload.data(), payload_item.payload_size);
    const std::string ordered = "\"category\":401,\"count\":7,\"payload\":\"" + encoded + "\"";
    const std::string reordered = "\"payload\":\"" + encoded + "\",\"count\":7,\"category\":401";
    std::string hash_order_json = json;
    const size_t ordered_pos = hash_order_json.find(ordered);
    CHECK(ordered_pos != std::string::npos);
    hash_order_json.replace(ordered_pos, ordered.size(), reordered);
    virtual_bag::State hash_order_parsed{};
    CHECK(virtual_bag::parse_state_json(hash_order_json.c_str(), &hash_order_parsed));
    CHECK_EQ(static_cast<int>(hash_order_parsed.items[1][2].payload_size), 19);
    CHECK(std::memcmp(hash_order_parsed.items[1][2].payload.data(), payload_item.payload.data(), 19) == 0);
}

static void test_virtual_bag_legacy_json() {
    virtual_bag::State state{};
    state.types = {4, 4, 4, 4, 4};
    virtual_bag::normalize(&state);
    virtual_bag::set_item(&state, 0, 0, 401, 7);
    virtual_bag::set_item(&state, 3, 1, 55, 2);
    const std::string json = virtual_bag::state_json(state);
    virtual_bag::State parsed{};
    CHECK(virtual_bag::parse_state_json(json.c_str(), &parsed));
    CHECK_EQ(parsed.items[0][0].category, 401);
    CHECK_EQ(parsed.items[0][0].count, 7);
    CHECK_EQ(static_cast<int>(parsed.items[0][0].payload_size), 0);
    CHECK_EQ(parsed.items[3][1].category, 55);
    CHECK(!parsed.pending.valid);
}

static void test_virtual_bag_normalize_payload() {
    virtual_bag::State state{};
    virtual_bag::normalize(&state);
    state.items[0][0].category = 401;
    state.items[0][0].count = 1;
    state.items[0][0].payload_size = 5;
    state.items[0][0].payload[0] = 4;
    virtual_bag::normalize(&state);
    CHECK_EQ(static_cast<int>(state.items[0][0].payload_size), 0);
    CHECK_EQ(state.items[0][0].category, 0);
    CHECK_EQ(state.items[0][0].count, 0);
}

static void test_virtual_bag_recovery() {
    virtual_bag::State state{};
    virtual_bag::normalize(&state);
    CHECK_EQ(virtual_bag::recovery_action(state, state.pending),
             virtual_bag::RecoveryAction::kNone);

    virtual_bag::PendingTransfer p{};
    p.valid = true;
    p.direction = virtual_bag::kTransferOriginalToExtension;
    p.src_bag = 0;
    p.src_slot = 3;
    p.dst_bag = 1;
    p.dst_slot = 5;
    virtual_bag::Item payload_item{};
    payload_item.category = 401;
    payload_item.count = 4;
    make_small_payload(&payload_item, 4);
    p.payload_size = payload_item.payload_size;
    p.payload = payload_item.payload;

    state.items[1][5] = payload_item;
    CHECK_EQ(virtual_bag::recovery_action(state, p), virtual_bag::RecoveryAction::kComplete);
    state.items[1][5] = {};
    CHECK_EQ(virtual_bag::recovery_action(state, p), virtual_bag::RecoveryAction::kRollback);

    virtual_bag::PendingTransfer p2{};
    p2.valid = true;
    p2.direction = virtual_bag::kTransferExtensionToOriginal;
    p2.src_bag = 1;
    p2.src_slot = 5;
    p2.dst_bag = 0;
    p2.dst_slot = 0;
    p2.payload_size = payload_item.payload_size;
    p2.payload = payload_item.payload;
    state.items[1][5] = payload_item;
    CHECK_EQ(virtual_bag::recovery_action(state, p2), virtual_bag::RecoveryAction::kRollback);
    state.items[1][5] = {};
    CHECK_EQ(virtual_bag::recovery_action(state, p2), virtual_bag::RecoveryAction::kComplete);

    p2.dst_bag = 6;
    CHECK_EQ(virtual_bag::recovery_action(state, p2), virtual_bag::RecoveryAction::kRollback);
}

// 存档预检样本：槽状态字节/失败码 → 五态判决（docs/system/save.md §4 阶段码全覆盖）
static void test_save_preflight_classify() {
    using V = SavePreflightVerdict;
    CHECK(save_preflight_classify(0, 0) == V::kMissing);   // 空槽/文件缺失
    CHECK(save_preflight_classify(2, 0) == V::kValid);     // 加载成功
    CHECK(save_preflight_classify(1, 3) == V::kIncompatible);  // 版本 >5 / 槽位号不匹配
    for (uint8_t c : {0, 1, 2, 4, 5, 6, 7})
        CHECK(save_preflight_classify(1, c) == V::kCorrupt);   // 其余阶段码均损坏
    CHECK(save_preflight_classify(3, 0) == V::kUnknown);   // 非法状态字节
    CHECK(save_preflight_classify(0xff, 0) == V::kUnknown);  // 非主菜单哨兵
    // 失败字节 bits[5:3] 为槽位号，不应干扰判决
    CHECK(save_preflight_classify(1, (2 << 3) | 3) == V::kIncompatible);
    CHECK(save_preflight_classify(1, (2 << 3) | 5) == V::kCorrupt);
}

static void test_save_preflight_stage() {
    CHECK_EQ(std::string(save_preflight_stage_name(0)), "load_data");
    CHECK_EQ(std::string(save_preflight_stage_name(1)), "block_table");
    CHECK_EQ(std::string(save_preflight_stage_name(2)), "information");
    CHECK_EQ(std::string(save_preflight_stage_name(3)), "validation");
    CHECK_EQ(std::string(save_preflight_stage_name(4)), "block_table");
    CHECK_EQ(std::string(save_preflight_stage_name(5)), "player");
    CHECK_EQ(std::string(save_preflight_stage_name(6)), "mercenary_slot");
    CHECK_EQ(std::string(save_preflight_stage_name(7)), "character");
    CHECK_EQ(std::string(save_preflight_stage_name(9)), "character");
    CHECK_EQ(std::string(save_preflight_stage_name(10)), "unknown");
    CHECK_EQ(std::string(save_preflight_stage_name(255)), "unknown");
}

static void test_save_preflight_json() {
    const std::string::size_type npos = std::string::npos;
    // 可进入样本：携带地图/角色诊断字段，无 stage
    std::string valid = save_preflight_json(1, 2, 0, 30, 27, 0);
    CHECK(valid.find("\"ok\":true") != npos);
    CHECK(valid.find("\"verdict\":\"valid\"") != npos);
    CHECK(valid.find("\"enter\":true") != npos);
    CHECK(valid.find("\"slot_state\":2") != npos);
    CHECK(valid.find("\"map_id\":30") != npos);
    CHECK(valid.find("\"hero_level\":27") != npos);
    CHECK(valid.find("\"hero_index\":0") != npos);
    CHECK(valid.find("\"stage\"") == npos);

    // 校验失败样本（load_data=解密/校验和层）：拒绝进入 + 阶段与错误码
    std::string corrupt = save_preflight_json(0, 1, 0, 0, 0, -1);
    CHECK(corrupt.find("\"verdict\":\"corrupt\"") != npos);
    CHECK(corrupt.find("\"enter\":false") != npos);
    CHECK(corrupt.find("\"stage\":\"load_data\"") != npos);
    CHECK(corrupt.find("\"error_code\":0") != npos);
    CHECK(corrupt.find("\"map_id\"") == npos);

    // 版本不兼容样本
    std::string incompat = save_preflight_json(2, 1, 3, 0, 0, -1);
    CHECK(incompat.find("\"verdict\":\"incompatible\"") != npos);
    CHECK(incompat.find("\"stage\":\"validation\"") != npos);
    CHECK(incompat.find("\"enter\":false") != npos);

    // 空槽样本
    std::string missing = save_preflight_json(1, 0, 0, 0, 0, -1);
    CHECK(missing.find("\"verdict\":\"missing\"") != npos);
    CHECK(missing.find("\"enter\":false") != npos);

    // 解析异常/未知样本：detail 透传（非主菜单拒刷新）
    std::string unknown = save_preflight_json(0, 0xff, 0, 0, 0, -1, "not in main menu (state=5)");
    CHECK(unknown.find("\"verdict\":\"unknown\"") != npos);
    CHECK(unknown.find("\"detail\":\"not in main menu (state=5)\"") != npos);
    std::string unknown_error = save_preflight_error_json(0, 0xff, 0);
    CHECK(unknown_error.find("\"ok\":false") != npos);
    CHECK(unknown_error.find("\"verdict\":\"unknown\"") != npos);
    CHECK(unknown_error.find("\"enter\":false") != npos);
    std::string escaped = save_preflight_json(0, 0xff, 0, 0, 0, -1, "state=5, \"world\"");
    CHECK(escaped.find("\\\"world\\\"") != npos);

    // enter_slot 拒绝体：错误信封 + 机器可读字段
    std::string err = save_preflight_error_json(0, 1, 2);
    CHECK(err.find("\"ok\":false") != npos);
    CHECK(err.find("\"error\":\"slot corrupt\"") != npos);
    CHECK(err.find("\"verdict\":\"corrupt\"") != npos);
    CHECK(err.find("\"stage\":\"information\"") != npos);
    CHECK(err.find("\"error_code\":2") != npos);
    CHECK(err.find("\"enter\":false") != npos);
    std::string semantic = save_preflight_semantic_error_json(0, "character", 7, "hero pointer is null");
    CHECK(semantic.find("\"verdict\":\"corrupt\"") != npos);
    CHECK(semantic.find("\"stage\":\"character\"") != npos);
    CHECK(semantic.find("hero pointer is null") != npos);
}

static void test_prepare_journal() {
    using namespace virtual_bag;
    auto base_record = []() {
        JournalRecord journal{};
        journal.valid = true;
        journal.stage = kJournalStagePrepared;
        journal.generation = 7;
        std::memcpy(journal.transaction_id, "j-1724800000-1", sizeof("j-1724800000-1"));
        journal.direction = kTransferOriginalToExtension;
        journal.src_bag = 0;
        journal.src_slot = 4;
        journal.dst_bag = 6 - 6;
        journal.dst_slot = 0;
        return journal;
    };

    CHECK(!valid_journal_record(JournalRecord{}));
    {
        JournalRecord journal = base_record();
        journal.valid = false;
        CHECK(!valid_journal_record(journal));
    }
    {
        JournalRecord journal = base_record();
        journal.transaction_id[0] = '\0';
        CHECK(!valid_journal_record(journal));
    }
    {
        JournalRecord journal = base_record();
        journal.stage = 3;
        CHECK(!valid_journal_record(journal));
    }
    {
        JournalRecord journal = base_record();
        journal.src_bag = 6;
        CHECK(!valid_journal_record(journal));
    }
    {
        JournalRecord journal = base_record();
        journal.direction = kTransferExtensionToOriginal;
        journal.src_bag = kBagCount;
        CHECK(!valid_journal_record(journal));
    }
    {
        JournalRecord journal = base_record();
        journal.payload_size = kPayloadHeaderSize - 1;
        CHECK(!valid_journal_record(journal));
    }

    State state{};
    CHECK(set_test_equipped(&state, 0, 2));
    Item source_item{};
    source_item.category = 7;
    source_item.count = 3;
    source_item.payload_size = kPayloadHeaderSize;
    source_item.payload[0] = static_cast<uint8_t>(kPayloadHeaderSize - 1);
    for (size_t i = 1; i < kPayloadHeaderSize; ++i) source_item.payload[i] = static_cast<uint8_t>(i);

    const WorldProbe source_held{true};
    const WorldProbe source_gone{false};

    {
        JournalRecord journal = base_record();
        journal.payload = source_item.payload;
        journal.payload_size = source_item.payload_size;
        CHECK(journal_recovery_action(state, journal, source_held) == JournalRecovery::kRollback);
        CHECK(journal_recovery_action(state, journal, source_gone) == JournalRecovery::kReplayToSidecar);
        journal.stage = kJournalStageOriginalSaved;
        CHECK(journal_recovery_action(state, journal, source_held) == JournalRecovery::kRollback);
        CHECK(journal_recovery_action(state, journal, source_gone) == JournalRecovery::kReplayToSidecar);
        State committed = state;
        committed.items[0][0] = source_item;
        CHECK(journal_recovery_action(committed, journal, source_gone) == JournalRecovery::kJustClear);
        journal.stage = kJournalStageSidecarCommitted;
        CHECK(journal_recovery_action(state, journal, source_held) == JournalRecovery::kJustClear);
    }
    {
        JournalRecord journal = base_record();
        journal.direction = kTransferExtensionToOriginal;
        journal.src_bag = 0;
        journal.src_slot = 0;
        journal.dst_bag = 1;
        journal.dst_slot = 5;
        State with_source = state;
        with_source.items[0][0] = source_item;
        const WorldProbe original_empty{false};
        const WorldProbe original_received{true};
        CHECK(journal_recovery_action(with_source, journal, original_empty) == JournalRecovery::kRollback);
        CHECK(journal_recovery_action(with_source, journal, original_received) ==
              JournalRecovery::kReplayToSidecar);
        State cleared = state;
        CHECK(journal_recovery_action(cleared, journal, original_received) == JournalRecovery::kJustClear);
    }

    {
        JournalRecord journal = base_record();
        journal.payload = source_item.payload;
        journal.payload_size = source_item.payload_size;
        journal.source_payload = source_item.payload;
        journal.source_payload_size = source_item.payload_size;
        const std::string encoded = journal_json(journal);
        JournalRecord parsed{};
        CHECK(parse_journal_json(encoded.c_str(), &parsed));
        CHECK_EQ(parsed.generation, journal.generation);
        CHECK_EQ((int)parsed.stage, (int)journal.stage);
        CHECK_EQ((int)parsed.direction, (int)journal.direction);
        CHECK_EQ(parsed.src_bag, journal.src_bag);
        CHECK_EQ(parsed.dst_slot, journal.dst_slot);
        CHECK(std::strcmp(parsed.transaction_id, journal.transaction_id) == 0);
        CHECK_EQ((int)parsed.payload_size, (int)journal.payload_size);
        CHECK(std::memcmp(parsed.payload.data(), journal.payload.data(), journal.payload_size) == 0);
        CHECK_EQ((int)parsed.source_payload_size, (int)journal.source_payload_size);
        JournalRecord corrupted{};
        CHECK(!parse_journal_json("{\"transactionId\":\"\"}", &corrupted));
        CHECK(!parse_journal_json("not json", &corrupted));
        CHECK(!parse_journal_json(
            "{\"transactionId\":\"x\",\"stage\":9,\"generation\":1,\"direction\":0,"
            "\"srcBag\":0,\"srcSlot\":0,\"dstBag\":0,\"dstSlot\":0}",
            &corrupted));
    }

    {
        PendingTransfer pending{};
        pending.valid = true;
        pending.direction = kTransferExtensionToOriginal;
        pending.src_bag = 2;
        pending.src_slot = 1;
        pending.dst_bag = 1;
        pending.dst_slot = 4;
        pending.payload_size = kPayloadHeaderSize;
        pending.payload[0] = static_cast<uint8_t>(kPayloadHeaderSize - 1);
        const JournalRecord journal = journal_from_pending(pending, 42);
        CHECK(!valid_journal_record(journal));
        JournalRecord identified = journal;
        std::memcpy(identified.transaction_id, "j-1724800000-9", sizeof("j-1724800000-9"));
        CHECK(valid_journal_record(identified));
        CHECK_EQ(journal.generation, 42u);
        CHECK_EQ((int)journal.direction, (int)kTransferExtensionToOriginal);
        CHECK_EQ(journal.src_bag, 2);
    }
}

static void test_ownership_ledger() {
    ownership::Ledger ledger{};
    uint32_t a = 0;
    uint32_t b = 0;

    CHECK(ownership::allocate(&ledger, &a) == ownership::Outcome::kOk);
    CHECK(ownership::allocate(&ledger, &b) == ownership::Outcome::kOk);
    CHECK(a != b);
    CHECK(ownership::live_state(ledger, a, ownership::State::kModuleOwned));
    CHECK_EQ(ledger.total_allocated, 2u);
    {
        const ownership::Audit report = ownership::audit(ledger);
        CHECK(report.balanced);
        CHECK_EQ(report.outstanding_objects, 2u);
        CHECK_EQ(report.outstanding_borrows, 0u);
    }

    CHECK(ownership::borrow_for_view(&ledger, a) == ownership::Outcome::kOk);
    CHECK(!ownership::live_state(ledger, a, ownership::State::kModuleOwned));
    CHECK(ownership::live_state(ledger, a, ownership::State::kBorrowedForView));
    CHECK(ownership::release(&ledger, a) == ownership::Outcome::kRejectUnknownHandle);
    CHECK(ownership::return_from_view(&ledger, a) == ownership::Outcome::kOk);
    CHECK(ownership::live_state(ledger, a, ownership::State::kModuleOwned));

    CHECK(ownership::handover_to_inventory(&ledger, a) == ownership::Outcome::kOk);
    CHECK(!ownership::live_state(ledger, a, ownership::State::kModuleOwned));
    CHECK(!ownership::live_state(ledger, a, ownership::State::kInventoryOwned));
    CHECK(ownership::release(&ledger, a) == ownership::Outcome::kRejectUnknownHandle);
    CHECK(ownership::handover_to_inventory(&ledger, a) == ownership::Outcome::kRejectUnknownHandle);

    CHECK(ownership::release(&ledger, b) == ownership::Outcome::kOk);
    {
        const ownership::Audit report = ownership::audit(ledger);
        CHECK(report.balanced);
        CHECK_EQ(ledger.total_released, 1u);
        CHECK_EQ(ledger.total_handed_over, 1u);
        CHECK_EQ(report.outstanding_objects, 0u);
        CHECK_EQ(report.outstanding_borrows, 0u);
        CHECK_EQ(report.inventory_owned, 1u);
        CHECK_EQ(report.live_handles, 1u);
    }

    uint32_t c = 0;
    CHECK(ownership::allocate(&ledger, &c) == ownership::Outcome::kOk);
    CHECK(c != b);
    CHECK(ownership::live_state(ledger, c, ownership::State::kModuleOwned));

    CHECK(ownership::allocate(nullptr, &c) == ownership::Outcome::kRejectInvalidState);
    CHECK(ownership::allocate(&ledger, nullptr) == ownership::Outcome::kRejectInvalidState);
    CHECK(ownership::release(&ledger, 0x7FFFFFFF) == ownership::Outcome::kRejectUnknownHandle);
}

int main() {
    test_json_escape();
    test_base64_decode();
    test_parse_int_field();
    test_tiles_parse();
    test_ownership_ledger();
    test_nav_bfs();
    test_nav_bfs_multi();
    test_stack_codec();
    test_virtual_bag_state();
    test_extension_bag_exit_rendering_state();
    test_prepare_journal();
    test_virtual_bag_base64();
    test_virtual_bag_payload_helpers();
    test_virtual_bag_merge_count();
    test_virtual_bag_mergeable_items();
    test_virtual_bag_json_roundtrip();
    test_virtual_bag_legacy_json();
    test_virtual_bag_normalize_payload();
    test_virtual_bag_recovery();
    test_save_preflight_classify();
    test_save_preflight_stage();
    test_save_preflight_json();

    std::printf("host_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
