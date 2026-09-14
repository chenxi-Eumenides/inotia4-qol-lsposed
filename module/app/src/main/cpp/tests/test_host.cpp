// host 纯函数单测（G4 裁决）：
// 覆盖 json_escape / base64_decode / parse_int_field / nav_bfs / nav_bfs_multi / tiles 解析。
// 全部编译真实被测源文件（非复制实现）：
//   - game_json.cpp   → json_escape（纯 STL）
//   - game_nav.cpp    → nav_bfs / nav_bfs_multi（BFS 纯算法，g_base=0 时 nav_unit_blocks 退化为空）
//   - game_tiles.cpp  → 本文件直接 #include，以访问匿名命名空间的 base64_decode/parse_int_field，
//                       并复用 set_static_tiles 注入瓦片矩阵
// 被测代码依赖的 Android 头由 stubs/android/log.h 覆盖；游戏内存符号由 test_stubs.cpp 提供。

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game_json.h"
#include "game_nav.h"
#include "game_save_preflight.h"
#include "game_tiles.h"
#include "feature/extension_bag/model/ownership_ledger.h"
#include "core/native/stack_codec.h"
#include "core/native/sell_price.h"
#include "feature/extension_bag/model/virtual_bag_state.h"
#include "feature/patch/inventory_find_item_poc.h"
#include "feature/save_backup/save_backup_bundle.h"
#include "feature/world_teleport/world_teleport_rules.h"
#include "../data/native/game_tiles.cpp"

extern void set_host_stack_limit_enabled(bool enabled);

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

static void* g_find_original_result = nullptr;
static void* g_find_extension_result = nullptr;
static int g_find_original_calls = 0;
static int g_find_extension_calls = 0;
static bool* g_find_recursive_guard = nullptr;

static void* p7_find_original_stub(int32_t) {
    ++g_find_original_calls;
    return g_find_original_result;
}

static void* p7_find_extension_stub(int32_t) {
    ++g_find_extension_calls;
    return g_find_extension_result;
}

static void* p7_find_recursive_original_stub(int32_t category) {
    ++g_find_original_calls;
    if (g_find_original_calls == 1 && g_find_recursive_guard != nullptr) {
        return inventory_find_item_original_first(category, p7_find_recursive_original_stub,
                                                   p7_find_extension_stub, *g_find_recursive_guard);
    }
    return nullptr;
}

static void test_p7_stage4_find_item_poc() {
    bool recursive_guard = false;
    g_find_original_result = reinterpret_cast<void*>(static_cast<uintptr_t>(0x101));
    g_find_extension_result = reinterpret_cast<void*>(static_cast<uintptr_t>(0x202));
    g_find_original_calls = 0;
    g_find_extension_calls = 0;
    CHECK(inventory_find_item_original_first(7, p7_find_original_stub, p7_find_extension_stub,
                                             recursive_guard) == g_find_original_result);
    CHECK_EQ(g_find_original_calls, 1);
    CHECK_EQ(g_find_extension_calls, 0);
    CHECK(!recursive_guard);

    g_find_original_result = nullptr;
    g_find_original_calls = 0;
    g_find_extension_calls = 0;
    CHECK(inventory_find_item_original_first(7, p7_find_original_stub, p7_find_extension_stub,
                                             recursive_guard) == g_find_extension_result);
    CHECK_EQ(g_find_original_calls, 1);
    CHECK_EQ(g_find_extension_calls, 1);
    CHECK(!recursive_guard);

    g_find_extension_result = nullptr;
    g_find_original_calls = 0;
    g_find_extension_calls = 0;
    CHECK(inventory_find_item_original_first(7, p7_find_original_stub, p7_find_extension_stub,
                                             recursive_guard) == nullptr);
    CHECK_EQ(g_find_original_calls, 1);
    CHECK_EQ(g_find_extension_calls, 1);
    CHECK(!recursive_guard);

    g_find_original_calls = 0;
    g_find_extension_calls = 0;
    g_find_recursive_guard = &recursive_guard;
    CHECK(inventory_find_item_original_first(7, p7_find_recursive_original_stub,
                                             p7_find_extension_stub, recursive_guard) == nullptr);
    CHECK_EQ(g_find_original_calls, 2);
    CHECK_EQ(g_find_extension_calls, 1);
    CHECK(!recursive_guard);
    g_find_recursive_guard = nullptr;

    recursive_guard = true;
    g_find_original_result = reinterpret_cast<void*>(static_cast<uintptr_t>(0x303));
    g_find_extension_calls = 0;
    CHECK(inventory_find_item_original_first(7, p7_find_original_stub, p7_find_extension_stub,
                                             recursive_guard) == g_find_original_result);
    CHECK_EQ(g_find_extension_calls, 0);
    recursive_guard = false;
}

static void test_world_teleport_target_map_id() {
    CHECK_EQ(world_teleport::target_map_id(87, 1), 88);
    CHECK_EQ(world_teleport::target_map_id(87, 10), 97);
    CHECK_EQ(world_teleport::target_map_id(87, -1), 86);
    CHECK_EQ(world_teleport::target_map_id(87, -10), 77);
    CHECK_EQ(world_teleport::target_map_id(414, 1), 0);
    CHECK_EQ(world_teleport::target_map_id(414, 10), 0);
    CHECK_EQ(world_teleport::target_map_id(405, 10), 0);
    CHECK_EQ(world_teleport::target_map_id(406, 10), 0);
    CHECK_EQ(world_teleport::target_map_id(0, -1), 414);
    CHECK_EQ(world_teleport::target_map_id(0, -10), 414);
    CHECK_EQ(world_teleport::target_map_id(5, -10), 414);
    CHECK_EQ(world_teleport::target_map_id(414, 0), 414);
    CHECK_EQ(world_teleport::target_map_id(0, 0), 0);
}

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
    // 原版袋对象 marker：bit0..24 容量位不被触碰，bit25..31 置 marker。
    const uint32_t bag_flags = 0x01A54321u | (0x55u << 25);
    const uint32_t marked = stack_codec::write_native_bag_object_marker(bag_flags);
    CHECK_EQ(marked & ((1u << 25) - 1u), bag_flags & ((1u << 25) - 1u));
    CHECK_EQ((marked >> stack_codec::kNativeBagObjectMarkerShift) & 0x7Fu, 1u);
}

// 布局层不变性（统一 S2、无版本标识）：s2_* 编解码本身与模式无关，同一数值在
// 堆叠上限启用/关闭两态下写入产物逐位一致、解码一致；切换模式不改变已编码值。
// 模式语义由 effective_* 模式感知层承载（见 test_stack_codec_effective_mode）。
static void test_stack_codec_mode_invariance() {
    const uint32_t low = 0x0003A55Fu;
    const uint32_t mixed = low | (0x5u << 22) | (0x2Au << 25);
    for (uint32_t count : {99u, 127u, 128u, 217u, 999u}) {
        set_host_stack_limit_enabled(false);
        const uint32_t encoded_disabled = stack_codec::s2_write_count(low, count);
        CHECK_EQ(stack_codec::s2_read_count(encoded_disabled), count);
        set_host_stack_limit_enabled(true);
        const uint32_t encoded_enabled = stack_codec::s2_write_count(low, count);
        // 写入产物与模式无关：逐位一致，解码一致。
        CHECK_EQ(encoded_enabled, encoded_disabled);
        CHECK_EQ(stack_codec::s2_read_count(encoded_enabled), count);
        // 含保留数据位的字段同样逐位一致（bits0–21 原样保留）。
        CHECK_EQ(stack_codec::s2_write_count(mixed, count),
                 stack_codec::s2_write_count(mixed, count));
        CHECK_EQ(stack_codec::s2_write_count(mixed, count) & ((1u << 22) - 1u), low);
        CHECK_EQ(stack_codec::s2_read_count(stack_codec::s2_write_count(mixed, count)), count);
        // 切换模式不改变已编码值：解码在两态下相同。
        set_host_stack_limit_enabled(false);
        CHECK_EQ(stack_codec::s2_read_count(encoded_enabled), count);
        set_host_stack_limit_enabled(true);
        CHECK_EQ(stack_codec::s2_read_count(encoded_disabled), count);
    }
    set_host_stack_limit_enabled(false);
}

// S2 编码（拆段 10-bit）独立测试段。
static void test_stack_codec_s2() {
    // 往返：bits0–21 干净基值 + 全部边界 count。
    const uint32_t low = 0x0003A55Fu;
    for (uint32_t count : {0u, 1u, 99u, 100u, 127u, 128u, 199u, 999u, 1023u}) {
        const uint32_t encoded = stack_codec::s2_write_count(low, count);
        CHECK_EQ(stack_codec::s2_read_count(encoded), count);
        CHECK_EQ(encoded & ((1u << 22) - 1u), low);
    }
    // 编码位布局抽查：count 拆段落位。
    CHECK_EQ(stack_codec::s2_write_count(0u, 128u), 1u << stack_codec::kS2ShiftA);
    CHECK_EQ(stack_codec::s2_write_count(0u, 127u), stack_codec::kS2MaskB);
    CHECK_EQ(stack_codec::s2_write_count(0u, 1023u),
             stack_codec::kS2MaskA | stack_codec::kS2MaskB);
    // 位保留：构造含 bits0–21 数据位与 marker 位（bits25–31）的值，写 count 后
    // bits0–21 不变；bits22–31 完全被 S2 编码接管（marker 区归 b 段，
    // 装备/宝石/袋容量类别禁止走 s2 写，见 stack_codec.h 门控注释）。
    const uint32_t mixed = 0x0003A55Fu | (0x7u << 22) | (0x55u << 25);
    for (uint32_t count : {0u, 1u, 128u, 999u, 1023u}) {
        const uint32_t encoded = stack_codec::s2_write_count(mixed, count);
        CHECK_EQ(encoded & ((1u << 22) - 1u), 0x0003A55Fu);
        CHECK_EQ(stack_codec::s2_read_count(encoded), count);
    }
    // split/combine 互逆：全编码域 0..1023。
    for (uint32_t count = 0; count <= stack_codec::kS2Max; ++count) {
        CHECK_EQ(stack_codec::s2_combine(stack_codec::s2_split_a(count),
                                         stack_codec::s2_split_b(count)),
                 count);
    }
    CHECK_EQ(stack_codec::s2_split_a(128u), 1u);
    CHECK_EQ(stack_codec::s2_split_b(128u), 0u);
    CHECK_EQ(stack_codec::s2_split_a(1023u), 7u);
    CHECK_EQ(stack_codec::s2_split_b(1023u), 127u);
    // clamp 两态：业务上限 999/99，编码上限 1023 只对操作层钳制生效。
    CHECK_EQ(stack_codec::s2_clamp(1023u, true), 999u);
    CHECK_EQ(stack_codec::s2_clamp(1000u, true), 999u);
    CHECK_EQ(stack_codec::s2_clamp(100u, true), 100u);
    CHECK_EQ(stack_codec::s2_clamp(0u, true), 0u);
    CHECK_EQ(stack_codec::s2_clamp(1023u, false), 99u);
    CHECK_EQ(stack_codec::s2_clamp(100u, false), 99u);
    CHECK_EQ(stack_codec::s2_clamp(99u, false), 99u);
}

// 模式感知读写（R-47 决策 b）：启用态 = S2 全量 128a+b（上限 999）；关闭态 =
// 低 7 位视图 b（上限 99），a（bits22–24）不读、不写、不参与运算，只原样保留。
// 核心断言：模式切换不丢值——canonical 199 关闭态读 71、重开读 199；关闭态写 99
// 只改 b → 重开读 128a+99；关闭态消耗按 b 计算、a 保留。
static void test_stack_codec_effective_mode() {
    const uint32_t low = 0x00012345u;  // bits0–21 保留数据位
    // canonical 199 = a=1、b=71。
    const uint32_t f199 = stack_codec::effective_write_count(low, 199u, true);
    CHECK_EQ(stack_codec::s2_read_count(f199), 199u);
    CHECK_EQ(f199 & ((1u << 22) - 1u), low);
    CHECK_EQ((f199 >> stack_codec::kS2ShiftA) & 0x7u, 1u);
    // 读：启用态全量 199；关闭态只读 b=71。
    CHECK_EQ(stack_codec::effective_read_count(f199, true), 199u);
    CHECK_EQ(stack_codec::effective_read_count(f199, false), 71u);
    // 127/128/99 的关闭态读：b 视图（128 的 b=0 → 关闭态读 0）。
    CHECK_EQ(stack_codec::effective_read_count(stack_codec::s2_write_count(low, 127u), false), 127u);
    CHECK_EQ(stack_codec::effective_read_count(stack_codec::s2_write_count(low, 128u), false), 0u);
    CHECK_EQ(stack_codec::effective_read_count(stack_codec::s2_write_count(low, 99u), false), 99u);
    // 关闭态写 99：只写 b、保留 a 与低位 → 重开读 128+99=227。
    const uint32_t off_write99 =
        stack_codec::effective_write_count(f199, stack_codec::effective_clamp(99u, false), false);
    CHECK_EQ(stack_codec::effective_read_count(off_write99, false), 99u);
    CHECK_EQ(stack_codec::s2_read_count(off_write99), 227u);
    CHECK_EQ(off_write99 & ((1u << 22) - 1u), low);
    CHECK_EQ((off_write99 >> stack_codec::kS2ShiftA) & 0x7u, 1u);
    // 关闭态消耗 1：b 71→70、a 保留 → 重开读 198。
    const uint32_t off_consume = stack_codec::effective_write_count(f199, 70u, false);
    CHECK_EQ(stack_codec::effective_read_count(off_consume, false), 70u);
    CHECK_EQ(stack_codec::s2_read_count(off_consume), 198u);
    // 模式切换不丢值：字段未动时，关闭态读 71 → 重开读 199。
    CHECK_EQ(stack_codec::effective_read_count(f199, false), 71u);
    CHECK_EQ(stack_codec::s2_read_count(f199), 199u);
    // clamp 两态：启用 999 / 关闭 99。
    CHECK_EQ(stack_codec::effective_clamp(1000u, true), 999u);
    CHECK_EQ(stack_codec::effective_clamp(142u, false), 99u);
    CHECK_EQ(stack_codec::effective_clamp(100u, false), 99u);
    CHECK_EQ(stack_codec::effective_clamp(99u, false), 99u);
    CHECK_EQ(stack_codec::effective_clamp(71u, false), 71u);
    // canonical int 域（descriptor）模式视图：199 off→71、128 off→0、重开原样。
    CHECK_EQ(stack_codec::effective_view_count(199u, true), 199u);
    CHECK_EQ(stack_codec::effective_view_count(199u, false), 71u);
    CHECK_EQ(stack_codec::effective_view_count(128u, false), 0u);
    CHECK_EQ(stack_codec::effective_view_count(99u, false), 99u);
}

static void test_sell_price_bounds() {
    int64_t price = 0;
    CHECK(sell_price::calculate(100, 99, false, &price));
    CHECK_EQ(price, 9900);
    CHECK(sell_price::calculate(100, 999, true, &price));
    CHECK_EQ(price, 69930);
    CHECK(!sell_price::calculate(-1, 1, false, &price));
    CHECK(!sell_price::calculate(sell_price::kMaxValue + 1, 1, false, &price));
    CHECK(!sell_price::calculate(sell_price::kMaxValue, 2, false, &price));
    CHECK(!sell_price::calculate(1, 0, false, &price));
    CHECK_EQ(stack_codec::s2_clamp(1000u, false), 99u);
    CHECK_EQ(stack_codec::s2_clamp(1000u, true), 999u);
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
    CHECK_EQ(state.info_bag, 0);
    CHECK_EQ(state.inspected, -1);
    state = {};
    CHECK(virtual_bag::set_test_equipped(&state, 0, 1));
    CHECK_EQ((int)state.types[0], 1);
    CHECK_EQ((int)state.capacities[0], 4);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(state.selected, 0);
    CHECK_EQ(state.inspected, -1);
    CHECK_EQ(virtual_bag::click(&state, 0), virtual_bag::ClickResult::kInspected);
    CHECK_EQ(state.info_bag, 0);
    CHECK_EQ(state.inspected, -1);
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

// S2 布局 payload（bits22–24=a、bits25–31=b）：数量位只有 S2 一种布局，统一用
// 本构造器生成可堆叠 payload。
static void make_s2_payload(virtual_bag::Item* item, uint32_t count) {
    item->payload_size = 19;
    item->payload[0] = static_cast<uint8_t>(item->payload_size - 1);
    const uint32_t count_u32 = stack_codec::s2_write_count(0x00012345u, count);
    for (size_t i = 0; i < 4; ++i) {
        item->payload[virtual_bag::kPayloadCountOffset + i] =
            static_cast<uint8_t>((count_u32 >> (8 * i)) & 0xFF);
    }
}

// payload UID 区间 [1,9)：构造两份不同实例身份的载荷。VM-39 根因是拾取对象与
// 扩展堆 UID 不同导致整份 payload memcmp 永不相等；UID 不得参与堆叠身份。
static void set_payload_uid(virtual_bag::Item* item, uint64_t uid) {
    for (size_t index = 0; index < virtual_bag::kPayloadUidSize; ++index) {
        item->payload[virtual_bag::kPayloadUidOffset + index] =
            static_cast<uint8_t>((uid >> (8 * index)) & 0xffu);
    }
}

// 带词缀链的 S2 payload（每节点 4B：u16 编码 + s16 值），用于词缀身份断言。
static void make_s2_payload_with_options(virtual_bag::Item* item, uint32_t count,
                                         size_t option_count) {
    item->payload_size = static_cast<uint16_t>(
        virtual_bag::kPayloadHeaderSize + 4 * option_count);
    item->payload[0] = static_cast<uint8_t>(item->payload_size - 1);
    const uint32_t count_u32 = stack_codec::s2_write_count(0x00012345u, count);
    for (size_t index = 0; index < 4; ++index) {
        item->payload[virtual_bag::kPayloadCountOffset + index] =
            static_cast<uint8_t>((count_u32 >> (8 * index)) & 0xFF);
    }
    for (size_t option = 0; option < option_count; ++option) {
        const size_t base = virtual_bag::kPayloadHeaderSize + option * 4;
        item->payload[base] = static_cast<uint8_t>(0x10 + option);
        item->payload[base + 1] = 0;
        item->payload[base + 2] = static_cast<uint8_t>(option);
        item->payload[base + 3] = 0;
    }
}

static stack_codec::CountEncoding host_category_uses_stack_count(int category) {
    return category == 333 ? stack_codec::CountEncoding::kNotEncoded
                           : stack_codec::CountEncoding::kEncoded;
}

static stack_codec::CountEncoding host_unknown_category(int) {
    return stack_codec::CountEncoding::kUnknown;
}

struct PayloadBridgeFixture {
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int save_result = static_cast<int>(virtual_bag::kPayloadHeaderSize);
    bool save_nonzero_tail = false;
    int load_result = 1;
    bool load_returns_item = true;
    int consumed = static_cast<int>(virtual_bag::kPayloadHeaderSize);
    int free_count = 0;
};

static PayloadBridgeFixture g_payload_bridge_fixture{};

static int payload_bridge_save(uint8_t* out, void* item) {
    auto* fixture = static_cast<PayloadBridgeFixture*>(item);
    std::memcpy(out, fixture->payload.data(), fixture->payload.size());
    if (fixture->save_nonzero_tail && fixture->save_result >= 0 &&
        fixture->save_result < static_cast<int>(virtual_bag::kSerializedItemProbeBuffer)) {
        out[fixture->save_result] = 0xa5;
    }
    return fixture->save_result;
}

static int payload_bridge_load(const uint8_t*, void** out, int* consumed) {
    *out = g_payload_bridge_fixture.load_returns_item ? &g_payload_bridge_fixture : nullptr;
    *consumed = g_payload_bridge_fixture.consumed;
    return g_payload_bridge_fixture.load_result;
}

static void payload_bridge_free(void* item) {
    if (item == &g_payload_bridge_fixture) ++g_payload_bridge_fixture.free_count;
}

static std::array<uint8_t, virtual_bag::kSerializedItemBuffer> make_payload_bytes(
    size_t payload_size, uint32_t count) {
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    for (size_t index = 1; index < payload_size; ++index) {
        payload[index] = static_cast<uint8_t>(index * 13u + 5u);
    }
    if (payload_size > 0) payload[0] = static_cast<uint8_t>(payload_size - 1);
    const uint32_t count_flags = stack_codec::s2_write_count(0x00012345u, count);
    for (size_t index = 0; index < 4; ++index) {
        payload[virtual_bag::kPayloadCountOffset + index] =
            static_cast<uint8_t>((count_flags >> (8 * index)) & 0xffu);
    }
    return payload;
}

static void configure_payload_bridge(
    const std::array<uint8_t, virtual_bag::kSerializedItemBuffer>& payload, int payload_size) {
    g_payload_bridge_fixture = {};
    g_payload_bridge_fixture.payload = payload;
    g_payload_bridge_fixture.save_result = payload_size;
    g_payload_bridge_fixture.consumed = payload_size;
}

static void test_virtual_bag_payload_bridge() {
    using namespace virtual_bag;

    for (const uint32_t count : {1u, 99u, 999u}) {
        const auto payload = make_payload_bytes(kPayloadHeaderSize, count);
        configure_payload_bridge(payload, static_cast<int>(kPayloadHeaderSize));
        const ManagedLoadResult result = load_item_payload_exact(
            payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
            payload_bridge_load, payload_bridge_free);
        CHECK(result.item == &g_payload_bridge_fixture);
        CHECK_EQ(result.failure, ManagedLoadFailure::kNone);
        CHECK_EQ(result.validation, PayloadValidation::kOk);
        uint32_t encoded_count = 0;
        for (size_t index = 0; index < 4; ++index) {
            encoded_count |= static_cast<uint32_t>(
                                 payload[kPayloadCountOffset + index]) << (8 * index);
        }
        CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(encoded_count)), static_cast<int>(count));
        CHECK_EQ(g_payload_bridge_fixture.free_count, 0);
    }

    auto complex_payload = make_payload_bytes(kPayloadHeaderSize, 1);
    complex_payload[1] = 0x92;
    complex_payload[7] = 0x3e;
    complex_payload[17] = 0xd4;
    configure_payload_bridge(complex_payload, static_cast<int>(kPayloadHeaderSize));
    ManagedLoadResult complex_result = load_item_payload_exact(
        complex_payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK(complex_result.item == &g_payload_bridge_fixture);
    CHECK(std::memcmp(g_payload_bridge_fixture.payload.data(), complex_payload.data(),
                      kPayloadHeaderSize) == 0);

    auto reordered_options = make_payload_bytes(kPayloadHeaderSize + 8, 1);
    const size_t first_option = kPayloadHeaderSize;
    const size_t second_option = first_option + 4;
    reordered_options[9] = 0x41;
    reordered_options[10] = 0x02;
    reordered_options[first_option] = 0x11;
    reordered_options[first_option + 1] = 0xaf;
    reordered_options[first_option + 2] = 0x33;
    reordered_options[first_option + 3] = 0x44;
    reordered_options[second_option] = 0x55;
    reordered_options[second_option + 1] = 0x66;
    reordered_options[second_option + 2] = 0x77;
    reordered_options[second_option + 3] = 0x88;
    configure_payload_bridge(reordered_options, static_cast<int>(reordered_options[0] + 1));
    g_payload_bridge_fixture.payload[first_option + 1] = 0xa0;
    ManagedLoadResult canonicalized_result = load_item_payload_exact(
        reordered_options.data(), static_cast<int>(reordered_options[0] + 1), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK(canonicalized_result.item == &g_payload_bridge_fixture);
    CHECK_EQ(canonicalized_result.failure, ManagedLoadFailure::kNone);

    configure_payload_bridge(reordered_options, static_cast<int>(reordered_options[0] + 1));
    std::swap_ranges(g_payload_bridge_fixture.payload.begin() + first_option,
                     g_payload_bridge_fixture.payload.begin() + second_option,
                     g_payload_bridge_fixture.payload.begin() + second_option);
    ManagedLoadResult reordered_result = load_item_payload_exact(
        reordered_options.data(), static_cast<int>(reordered_options[0] + 1), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(reordered_result.failure, ManagedLoadFailure::kRoundTripMismatch);

    configure_payload_bridge(reordered_options, static_cast<int>(reordered_options[0] + 1));
    g_payload_bridge_fixture.payload[second_option + 2] ^= 1;
    ManagedLoadResult changed_option_result = load_item_payload_exact(
        reordered_options.data(), static_cast<int>(reordered_options[0] + 1), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(changed_option_result.failure, ManagedLoadFailure::kRoundTripMismatch);

    const auto max_payload = make_payload_bytes(kMaxSerializedItem, 999);
    configure_payload_bridge(max_payload, static_cast<int>(kMaxSerializedItem));
    ManagedLoadResult max_result = load_item_payload_exact(
        max_payload.data(), static_cast<int>(kMaxSerializedItem), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK(max_result.item == &g_payload_bridge_fixture);
    CHECK_EQ(max_result.failure, ManagedLoadFailure::kNone);

    auto invalid_payload = make_payload_bytes(kPayloadHeaderSize, 1);
    const auto unchanged_payload = invalid_payload;
    ManagedLoadResult empty_result = load_item_payload_exact(
        invalid_payload.data(), 0, payload_bridge_save, payload_bridge_load, payload_bridge_free);
    CHECK_EQ(empty_result.failure, ManagedLoadFailure::kInvalidPayload);
    CHECK_EQ(empty_result.validation, PayloadValidation::kMissing);
    CHECK(std::memcmp(invalid_payload.data(), unchanged_payload.data(), invalid_payload.size()) == 0);

    ManagedLoadResult truncated_result = load_item_payload_exact(
        invalid_payload.data(), static_cast<int>(kPayloadHeaderSize - 1), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(truncated_result.failure, ManagedLoadFailure::kInvalidPayload);
    CHECK_EQ(truncated_result.validation, PayloadValidation::kLengthOutOfRange);

    invalid_payload[0] = 0;
    ManagedLoadResult prefix_result = load_item_payload_exact(
        invalid_payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(prefix_result.validation, PayloadValidation::kLengthPrefixMismatch);

    invalid_payload = make_payload_bytes(kPayloadHeaderSize, 1);
    invalid_payload[kPayloadHeaderSize] = 0x7f;
    ManagedLoadResult tail_result = load_item_payload_exact(
        invalid_payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(tail_result.validation, PayloadValidation::kNonZeroTail);

    invalid_payload = make_payload_bytes(kMaxSerializedItem, 1);
    invalid_payload[0] = 0xff;
    ManagedLoadResult oversize_result = load_item_payload_exact(
        invalid_payload.data(), static_cast<int>(kSerializedItemBuffer), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(oversize_result.validation, PayloadValidation::kLengthOutOfRange);

    const auto valid_payload = make_payload_bytes(kPayloadHeaderSize, 1);
    configure_payload_bridge(valid_payload, static_cast<int>(kPayloadHeaderSize));
    g_payload_bridge_fixture.load_result = 0;
    ManagedLoadResult load_failure = load_item_payload_exact(
        valid_payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(load_failure.failure, ManagedLoadFailure::kLoadFailed);
    CHECK_EQ(g_payload_bridge_fixture.free_count, 1);

    configure_payload_bridge(valid_payload, static_cast<int>(kPayloadHeaderSize));
    g_payload_bridge_fixture.load_returns_item = false;
    ManagedLoadResult missing_output = load_item_payload_exact(
        valid_payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(missing_output.failure, ManagedLoadFailure::kMissingOutput);
    CHECK_EQ(g_payload_bridge_fixture.free_count, 0);

    configure_payload_bridge(valid_payload, static_cast<int>(kPayloadHeaderSize));
    g_payload_bridge_fixture.consumed = static_cast<int>(kPayloadHeaderSize - 1);
    ManagedLoadResult consumed_mismatch = load_item_payload_exact(
        valid_payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(consumed_mismatch.failure, ManagedLoadFailure::kConsumedMismatch);
    CHECK_EQ(g_payload_bridge_fixture.free_count, 1);

    configure_payload_bridge(valid_payload, static_cast<int>(kPayloadHeaderSize));
    g_payload_bridge_fixture.payload[7] ^= 1;
    ManagedLoadResult round_trip_mismatch = load_item_payload_exact(
        valid_payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(round_trip_mismatch.failure, ManagedLoadFailure::kRoundTripMismatch);
    CHECK_EQ(g_payload_bridge_fixture.free_count, 1);

    configure_payload_bridge(valid_payload, static_cast<int>(kPayloadHeaderSize));
    g_payload_bridge_fixture.save_nonzero_tail = true;
    ManagedLoadResult reserialize_rejected = load_item_payload_exact(
        valid_payload.data(), static_cast<int>(kPayloadHeaderSize), payload_bridge_save,
        payload_bridge_load, payload_bridge_free);
    CHECK_EQ(reserialize_rejected.failure, ManagedLoadFailure::kReserializeRejected);
    CHECK_EQ(reserialize_rejected.validation, PayloadValidation::kNonZeroTail);
    CHECK_EQ(g_payload_bridge_fixture.free_count, 1);

    PendingTransfer pending{};
    pending.valid = true;
    pending.direction = kTransferOriginalToExtension;
    pending.src_bag = 0;
    pending.src_slot = 0;
    pending.dst_bag = 0;
    pending.dst_slot = 0;
    pending.payload = valid_payload;
    pending.payload_size = static_cast<uint16_t>(kPayloadHeaderSize);
    CHECK(valid_pending_transfer_payload(pending));
    pending.payload[kPayloadHeaderSize] = 0x01;
    CHECK(!valid_pending_transfer_payload(pending));
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
    make_s2_payload(&item, 3);
    CHECK(virtual_bag::valid_payload(item));
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(virtual_bag::payload_count(item))), 3);
    CHECK(virtual_bag::patch_payload_count(&item, 7, host_category_uses_stack_count));
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(virtual_bag::payload_count(item))), 7);
    CHECK_EQ(virtual_bag::payload_count(item) & ~(stack_codec::kS2MaskA | stack_codec::kS2MaskB),
             0x00012345u);
    CHECK(virtual_bag::patch_payload_count(&item, 999, host_category_uses_stack_count));
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(virtual_bag::payload_count(item))), 999);
    virtual_bag::Item equipment = item;
    equipment.category = 333;
    const auto equipment_payload = equipment.payload;
    CHECK(!virtual_bag::patch_payload_count(&equipment, 7, host_category_uses_stack_count));
    CHECK(std::memcmp(equipment.payload.data(), equipment_payload.data(), equipment_payload.size()) == 0);
    CHECK(!virtual_bag::patch_payload_count(&item, 7, host_unknown_category));
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

// 模式感知操作（R-47 决策 b）：patch_payload_count 关闭态只写 payload b 位、
// 保留 a；descriptor canonical 回读完整值；合并按 b 视图计算且目标 a 保留。
static void test_virtual_bag_mode_aware_ops() {
    virtual_bag::Item item{};
    item.category = 401;
    make_s2_payload(&item, 199);
    item.count = 199;
    // 关闭态写 99：payload b=99、a 与低位保留（canonical 227）。
    CHECK(virtual_bag::patch_payload_count(&item, 99, host_category_uses_stack_count, false));
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(virtual_bag::payload_count(item))), 227);
    CHECK_EQ(virtual_bag::payload_count(item) & ~(stack_codec::kS2MaskA | stack_codec::kS2MaskB),
             0x00012345u);
    // descriptor 回写 canonical：关闭态操作后重开读回完整值。
    item.count = static_cast<int>(
        stack_codec::s2_read_count(virtual_bag::payload_count(item)));
    CHECK_EQ(item.count, 227);
    CHECK_EQ(stack_codec::effective_view_count(static_cast<uint32_t>(item.count), false), 99u);
    // 默认参数（canonical 语义）写与既有行为一致。
    CHECK(virtual_bag::patch_payload_count(&item, 999, host_category_uses_stack_count));
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(virtual_bag::payload_count(item))), 999);
    // 关闭态合并：视图 71+71=142 → clamp 99；目标 a 保留 → canonical 227。
    virtual_bag::Item existing{};
    virtual_bag::Item source{};
    existing.category = 401;
    source.category = 401;
    make_s2_payload(&existing, 199);
    make_s2_payload(&source, 199);
    existing.count = 199;
    source.count = 199;
    const uint32_t existing_view = stack_codec::effective_view_count(199u, false);
    const uint32_t source_view = stack_codec::effective_view_count(199u, false);
    CHECK_EQ(existing_view, 71u);
    CHECK_EQ(source_view, 71u);
    CHECK_EQ(virtual_bag::merge_count(static_cast<int>(existing_view),
                                      static_cast<int>(source_view), false), 99u);
    CHECK(virtual_bag::patch_payload_count(&existing, 99, host_category_uses_stack_count, false));
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(virtual_bag::payload_count(existing))),
             227);
    // 启用态同输入：全量合并 71+71=142 < 999 → payload canonical 142（a=1）。
    CHECK_EQ(virtual_bag::merge_count(static_cast<int>(existing_view),
                                      static_cast<int>(source_view), true), 142u);
    CHECK(virtual_bag::patch_payload_count(&existing, 142, host_category_uses_stack_count, true));
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(virtual_bag::payload_count(existing))),
             142);
}

static void test_virtual_bag_mergeable_items() {
    virtual_bag::Item existing{};
    virtual_bag::Item source{};
    existing.category = 401;
    source.category = 401;
    existing.count = 3;
    source.count = 5;
    make_s2_payload(&existing, existing.count);
    make_s2_payload(&source, source.count);
    // 不同 UID + 不同数量位（S2 a/b 段）→ 堆叠身份一致，可并（VM-39 根因回归）。
    set_payload_uid(&existing, 0x1122334455667788ull);
    set_payload_uid(&source, 0x99aabbccddeeff00ull);
    CHECK(virtual_bag::mergeable_items(existing, source, host_category_uses_stack_count));
    CHECK(virtual_bag::same_extension_bag_mergeable_items(existing, source,
                                                           host_category_uses_stack_count));
    // UID 任意位翻转不影响身份（排除实例字段）。
    source.payload[virtual_bag::kPayloadUidOffset] ^= 0x5a;
    CHECK(virtual_bag::mergeable_items(existing, source, host_category_uses_stack_count));
    // 类别不同 → 不可并。
    source.category = 402;
    CHECK(!virtual_bag::mergeable_items(existing, source, host_category_uses_stack_count));
    source.category = existing.category;
    // 镶嵌位（payload[16]）不同 → 不可并。
    source.payload[16] ^= 1;
    CHECK(!virtual_bag::mergeable_items(existing, source, host_category_uses_stack_count));
    CHECK(!virtual_bag::same_extension_bag_mergeable_items(existing, source,
                                                            host_category_uses_stack_count));
    source.payload[16] ^= 1;
    CHECK(virtual_bag::mergeable_items(existing, source, host_category_uses_stack_count));
    // 词缀链：同条数、仅 UID 不同 → 可并；词缀值不同 → 不可并；条数不同 → 不可并。
    virtual_bag::Item option_existing{};
    option_existing.category = 401;
    option_existing.count = 2;
    make_s2_payload_with_options(&option_existing, 2, 2);
    virtual_bag::Item option_source = option_existing;
    set_payload_uid(&option_source, 0x0102030405060708ull);
    CHECK(virtual_bag::mergeable_items(option_existing, option_source,
                                       host_category_uses_stack_count));
    option_source.payload[virtual_bag::kPayloadHeaderSize + 2] ^= 0x11;
    CHECK(!virtual_bag::mergeable_items(option_existing, option_source,
                                        host_category_uses_stack_count));
    virtual_bag::Item option_extra{};
    option_extra.category = 401;
    option_extra.count = 2;
    make_s2_payload_with_options(&option_extra, 2, 1);
    CHECK(!virtual_bag::mergeable_items(option_existing, option_extra,
                                        host_category_uses_stack_count));
    // 空载荷（无有效长度前缀）→ 不可并（fail-closed）。
    virtual_bag::Item legacy_existing{existing.category, existing.count};
    virtual_bag::Item empty_payload{existing.category, source.count};
    CHECK(!virtual_bag::mergeable_items(legacy_existing, empty_payload,
                                        host_category_uses_stack_count));
    virtual_bag::Item equipment_existing{};
    virtual_bag::Item equipment_source{};
    equipment_existing.category = 333;
    equipment_source.category = 333;
    equipment_existing.count = 1;
    equipment_source.count = 1;
    make_s2_payload(&equipment_existing, 100);
    make_s2_payload(&equipment_source, 100);
    CHECK(!virtual_bag::same_extension_bag_mergeable_items(
        equipment_existing, equipment_source, host_category_uses_stack_count));
    CHECK(virtual_bag::same_extension_bag_merge_allowed(0, 0));
    CHECK(!virtual_bag::same_extension_bag_merge_allowed(0, 1));
    // 合并判定优先于交换；交换只在目标非空且不满足合并条件时进入。
    CHECK(virtual_bag::same_extension_bag_mergeable_items(existing, existing,
                                                          host_category_uses_stack_count));
    CHECK(virtual_bag::extension_swap_tokens_available(false, false, false, false));
    CHECK(!virtual_bag::extension_swap_tokens_available(true, false, false, false));
    CHECK(!virtual_bag::extension_swap_tokens_available(false, false, false, true));
    CHECK(!virtual_bag::projection_drop_context_matches(1, 0, true, true, true));
    CHECK(!virtual_bag::projection_drop_context_matches(0, 0, false, true, true));
    void* materialized_object = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
    CHECK_EQ(virtual_bag::handle_if_object_matches(materialized_object, materialized_object, 77), 77u);
    CHECK_EQ(virtual_bag::handle_if_object_matches(materialized_object, nullptr, 77), 0u);
    CHECK(virtual_bag::extension_source_protection_required(true, false));
    CHECK(virtual_bag::extension_source_protection_required(true, true));
    CHECK(virtual_bag::extension_source_protection_required(false, true));
    CHECK(!virtual_bag::extension_source_protection_required(false, false));
    CHECK(virtual_bag::projected_release_owns_event(0x18, true));
    CHECK(!virtual_bag::projected_release_owns_event(0x17, true));
    CHECK(!virtual_bag::projected_release_owns_event(0x18, false));
    std::array<void*, virtual_bag::kPhysicalInventorySnapshotSlots> before{};
    auto after = before;
    CHECK(!virtual_bag::physical_inventory_snapshot_changed(before, after));
    after[5 * virtual_bag::kSlotCount + 3] = materialized_object;
    CHECK(virtual_bag::physical_inventory_snapshot_changed(before, after));
    after = before;
    CHECK(!virtual_bag::physical_inventory_snapshot_changed(before, after));
}

// VM-37：出售/拆堆直接位读重定向后的解码语义。重定向把调用点改为
// `bl ITEM_GetCumulateCount(item)`，其值等价于 effective_read_count：启用态
// 128a+b 全量、关闭态 b（与原版 GetBitValue(31,25) 逐位一致）。同时守卫
// 「GetBitValue start 25→22」的伪修复：十位窗口 (v>>22)&0x3FF 是 `a+8b`，
// 与 `a*128+b` 在 count≥128 时不等价。
static void test_sell_divide_s2_read_window() {
    const uint32_t samples[] = {0u, 1u, 71u, 72u, 99u, 127u, 128u, 150u, 199u, 200u,
                                999u, 1023u};
    for (uint32_t count : samples) {
        const uint32_t field = stack_codec::s2_write_count(0x00012345u, count);
        CHECK_EQ(stack_codec::effective_read_count(field, true), count);
        CHECK_EQ(stack_codec::effective_read_count(field, false), (field >> 25) & 0x7Fu);
    }
    // 伪修复反例：十位窗口不是全量（128/150/199/200/999）。
    const uint32_t unequal[] = {128u, 150u, 199u, 200u, 999u};
    for (uint32_t count : unequal) {
        const uint32_t field = stack_codec::s2_write_count(0x00012345u, count);
        CHECK(((field >> 22) & 0x3FFu) != stack_codec::s2_read_count(field));
    }
    // 200 = a1+b72：启用态读 200，关闭态读 72（与原版 b 读一致）。
    const uint32_t field200 = stack_codec::s2_write_count(0x00012345u, 200u);
    CHECK_EQ(stack_codec::effective_read_count(field200, true), 200u);
    CHECK_EQ(stack_codec::effective_read_count(field200, false), 72u);
    CHECK_EQ((field200 >> 25) & 0x7Fu, 72u);
}

// VM-39：SaveItem 漏斗的同类堆合并计划（backup 前 merge_native_item 与 backup
// 失败后 adopt 共用同一 adopt_merge_into 判据；原版有空槽时也不进原版新格）。
static void test_adopt_merge_plan() {
    // 同类堆：existing 150（a=1,b=22）+ 拾取 1 → 合并 151，canonical 回填。
    virtual_bag::Item existing{};
    existing.category = 401;
    existing.count = 150;
    make_s2_payload(&existing, 150);
    virtual_bag::Item incoming{};
    incoming.category = 401;
    incoming.count = 1;
    make_s2_payload(&incoming, 1);
    virtual_bag::Item merged{};
    CHECK(virtual_bag::adopt_merge_into(existing, incoming,
                                        host_category_uses_stack_count, true, &merged));
    CHECK_EQ(merged.count, 151);
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(virtual_bag::payload_count(merged))),
             151);
    CHECK_EQ(merged.payload_size, existing.payload_size);
    // 不同 UID（拾取对象 vs 扩展堆实例）→ 身份一致，仍可并（VM-39 根因回归）。
    virtual_bag::Item other_uid = incoming;
    set_payload_uid(&existing, 0xaaaabbbbccccddddull);
    set_payload_uid(&other_uid, 0x0102030405060708ull);
    CHECK(virtual_bag::adopt_merge_into(existing, other_uid,
                                        host_category_uses_stack_count, true, &merged));
    // 镶嵌位不同（身份不同）→ 不合并。
    virtual_bag::Item other_payload = incoming;
    other_payload.payload[16] ^= 1;
    CHECK(!virtual_bag::adopt_merge_into(existing, other_payload,
                                         host_category_uses_stack_count, true, &merged));
    // 启用态超上限（999）：150 + 850 → 1000 > 999 → 不合并；恰好 999 可并。
    virtual_bag::Item big{};
    big.category = 401;
    big.count = 850;
    make_s2_payload(&big, 850);
    CHECK(!virtual_bag::adopt_merge_into(existing, big,
                                         host_category_uses_stack_count, true, &merged));
    big.count = 849;
    make_s2_payload(&big, 849);
    CHECK(virtual_bag::adopt_merge_into(existing, big,
                                        host_category_uses_stack_count, true, &merged));
    CHECK_EQ(merged.count, 999);
    // 关闭态：视图 b 相加、上限 99（a 不参与运算，R-47）。existing canonical
    // 150（a=1,b=22）保留 a，只改 b=23 → canonical 128+23=151，视图 23。
    virtual_bag::Item off_existing{};
    off_existing.category = 401;
    off_existing.count = 150;  // 关闭态视图 = 22
    make_s2_payload(&off_existing, 150);
    virtual_bag::Item off_incoming{};
    off_incoming.category = 401;
    off_incoming.count = 1;
    make_s2_payload(&off_incoming, 1);
    CHECK(virtual_bag::adopt_merge_into(off_existing, off_incoming,
                                        host_category_uses_stack_count, false, &merged));
    CHECK_EQ(merged.count, 151);  // canonical 保留 a
    CHECK_EQ(stack_codec::effective_view_count(static_cast<uint32_t>(merged.count), false),
             23u);
    // 关闭态超上限：22 + 78 = 100 > 99 → 不合并。
    virtual_bag::Item off_big{};
    off_big.category = 401;
    off_big.count = 78;
    make_s2_payload(&off_big, 78);
    CHECK(!virtual_bag::adopt_merge_into(off_existing, off_big,
                                         host_category_uses_stack_count, false, &merged));
    // 非可堆叠类别 → mergeable 门控拒绝（R-46 fail-closed）。
    virtual_bag::Item equip_existing{};
    equip_existing.category = 333;
    equip_existing.count = 1;
    make_s2_payload(&equip_existing, 1);
    CHECK(!virtual_bag::adopt_merge_into(equip_existing, equip_existing,
                                         host_category_uses_stack_count, true, &merged));
}

// R-53：入库落位计划纯函数（单一 owner 的唯一决策 seam）。两路径同契约：
// 有可并堆 → kMerged（与 allow_new_slot 无关）；无堆且 allow_new_slot=false →
// kNoSpace（保持 original-first）；allow_new_slot=true 且有空槽 → kPlacedNewSlot；
// 无空槽 → kNoSpace；source 非法 → kRejected（fail-closed）。
static void test_place_plan() {
    using Outcome = virtual_bag::PlaceOutcome;
    using ItemGrid =
        std::array<std::array<virtual_bag::Item, virtual_bag::kSlotCount>, virtual_bag::kBagCount>;
    ItemGrid items{};
    std::array<uint8_t, virtual_bag::kBagCount> types{};
    std::array<uint8_t, virtual_bag::kBagCount> capacities{};

    // 同身份（payload count 位不同/a≠0）：existing canonical 150 + 拾取 1 → 151。
    types[0] = 4;
    capacities[0] = 16;
    virtual_bag::Item existing{};
    existing.category = 401;
    existing.count = 150;
    make_s2_payload(&existing, 150);
    items[0][0] = existing;
    virtual_bag::Item incoming{};
    incoming.category = 401;
    incoming.count = 1;
    make_s2_payload(&incoming, 1);

    const virtual_bag::PlacePlan merged_merge = virtual_bag::place_plan(
        items, types, capacities, incoming, /*allow_new_slot=*/false, true,
        host_category_uses_stack_count);
    CHECK(merged_merge.outcome == Outcome::kMerged);
    CHECK_EQ(merged_merge.bag, 0);
    CHECK_EQ(merged_merge.slot, 0);
    CHECK_EQ(merged_merge.merged.count, 151);
    CHECK_EQ(stack_codec::effective_view_count(
                 static_cast<uint32_t>(merged_merge.merged.count), false), 23u);
    // 同一输入 allow_new_slot=true 仍优先合并（两路径同契约）。
    const virtual_bag::PlacePlan merged_adopt = virtual_bag::place_plan(
        items, types, capacities, incoming, /*allow_new_slot=*/true, true,
        host_category_uses_stack_count);
    CHECK(merged_adopt.outcome == Outcome::kMerged);
    CHECK_EQ(merged_adopt.slot, 0);

    // 异身份（镶嵌位不同）拒并：allow_new_slot=false → kNoSpace。
    virtual_bag::Item other = incoming;
    other.payload[16] ^= 1;
    const virtual_bag::PlacePlan rejected_merge = virtual_bag::place_plan(
        items, types, capacities, other, /*allow_new_slot=*/false, true,
        host_category_uses_stack_count);
    CHECK(rejected_merge.outcome == Outcome::kNoSpace);
    // allow_new_slot=true → 落到第一个空槽（slot 1）。
    const virtual_bag::PlacePlan new_slot = virtual_bag::place_plan(
        items, types, capacities, other, /*allow_new_slot=*/true, true,
        host_category_uses_stack_count);
    CHECK(new_slot.outcome == Outcome::kPlacedNewSlot);
    CHECK_EQ(new_slot.bag, 0);
    CHECK_EQ(new_slot.slot, 1);

    // 无空槽：16 槽全部为不可并的异身份 payload → kNoSpace（含 allow_new_slot）。
    ItemGrid full{};
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        virtual_bag::Item filler{};
        filler.category = 401;
        filler.count = 1;
        make_s2_payload(&filler, 1);
        filler.payload[16] ^= static_cast<uint8_t>(slot + 1);  // 与 source 身份不同
        full[0][slot] = filler;
    }
    const virtual_bag::PlacePlan no_space = virtual_bag::place_plan(
        full, types, capacities, incoming, /*allow_new_slot=*/true, true,
        host_category_uses_stack_count);
    CHECK(no_space.outcome == Outcome::kNoSpace);

    // 未装备袋不参与落位：types 全 0 且有空槽仍 kNoSpace（维度保证不触物理袋 5）。
    std::array<uint8_t, virtual_bag::kBagCount> no_types{};
    const virtual_bag::PlacePlan unequipped = virtual_bag::place_plan(
        items, no_types, capacities, incoming, /*allow_new_slot=*/true, true,
        host_category_uses_stack_count);
    CHECK(unequipped.outcome == Outcome::kNoSpace);

    // source 非法（无 payload）→ kRejected（fail-closed）。
    virtual_bag::Item invalid{};
    invalid.category = 401;
    invalid.count = 1;
    const virtual_bag::PlacePlan rejected_source = virtual_bag::place_plan(
        items, types, capacities, invalid, /*allow_new_slot=*/true, true,
        host_category_uses_stack_count);
    CHECK(rejected_source.outcome == Outcome::kRejected);

    // 失败路径回滚纯模型：generation 失败 → descriptor 与失败前一致（category/
    // count/payload_size/payload 逐字段相同）；成功 → 写出 source 字段。
    const virtual_bag::Item rolled_back =
        virtual_bag::apply_new_slot_descriptor(existing, incoming, false);
    CHECK_EQ(rolled_back.category, existing.category);
    CHECK_EQ(rolled_back.count, existing.count);
    CHECK_EQ(rolled_back.payload_size, existing.payload_size);
    CHECK(std::memcmp(rolled_back.payload.data(), existing.payload.data(),
                      existing.payload_size) == 0);
    const virtual_bag::Item committed =
        virtual_bag::apply_new_slot_descriptor(existing, incoming, true);
    CHECK_EQ(committed.category, incoming.category);
    CHECK_EQ(committed.count, incoming.count);
    CHECK_EQ(committed.payload_size, incoming.payload_size);
    CHECK(std::memcmp(committed.payload.data(), incoming.payload.data(),
                      incoming.payload_size) == 0);
}

// VM-40：ext→orig 事务的投影宿主守卫判定。
static void test_ext2orig_host_guards() {
    // 宿主容量字恢复：仅当投影安装中且目标袋 == 投影宿主袋。
    CHECK(virtual_bag::ext2orig_requires_host_capacity_restore(true, 0, 0));
    CHECK(virtual_bag::ext2orig_requires_host_capacity_restore(true, 2, 2));
    CHECK(!virtual_bag::ext2orig_requires_host_capacity_restore(true, 1, 0));
    CHECK(!virtual_bag::ext2orig_requires_host_capacity_restore(false, 0, 0));
    CHECK(!virtual_bag::ext2orig_requires_host_capacity_restore(true, 0, -1));
    // 显示袋切换：投影安装中禁止（宿主身份 R-26/R-27），非投影（API）保留。
    CHECK(!virtual_bag::ext2orig_should_switch_display_bag(true));
    CHECK(virtual_bag::ext2orig_should_switch_display_bag(false));
}

static void test_virtual_bag_json_roundtrip() {
    virtual_bag::State state{};
    state.types = {4, 4, 4, 4, 4};
    virtual_bag::normalize(&state);
    virtual_bag::Item payload_item{};
    payload_item.category = 401;
    payload_item.count = 7;
    make_s2_payload(&payload_item, 7);
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

static void test_virtual_bag_json_count_clamp() {
    virtual_bag::State oversized_state{};
    oversized_state.items[0][0].category = 401;
    oversized_state.items[0][0].count = 1000;
    make_s2_payload(&oversized_state.items[0][0], 1000);

    virtual_bag::State oversized_parsed{};
    CHECK(virtual_bag::parse_state_json(
        virtual_bag::state_json(oversized_state).c_str(), &oversized_parsed,
        host_category_uses_stack_count));
    CHECK_EQ(oversized_parsed.items[0][0].count, 999);
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(
                  virtual_bag::payload_count(oversized_parsed.items[0][0]))), 999);

    virtual_bag::State cross_config_state{};
    cross_config_state.items[0][0].category = 401;
    cross_config_state.items[0][0].count = 199;
    make_s2_payload(&cross_config_state.items[0][0], 199);

    set_host_stack_limit_enabled(true);
    const std::string enabled_json = virtual_bag::state_json(cross_config_state);
    set_host_stack_limit_enabled(false);
    virtual_bag::State parsed_disabled{};
    CHECK(virtual_bag::parse_state_json(
        enabled_json.c_str(), &parsed_disabled, host_category_uses_stack_count));
    CHECK_EQ(parsed_disabled.items[0][0].count, 199);
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(
                  virtual_bag::payload_count(parsed_disabled.items[0][0]))), 199);

    set_host_stack_limit_enabled(false);
    const std::string disabled_json = virtual_bag::state_json(cross_config_state);
    set_host_stack_limit_enabled(true);
    virtual_bag::State parsed_enabled{};
    CHECK(virtual_bag::parse_state_json(
        disabled_json.c_str(), &parsed_enabled, host_category_uses_stack_count));
    CHECK_EQ(parsed_enabled.items[0][0].count, 199);
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(
                  virtual_bag::payload_count(parsed_enabled.items[0][0]))), 199);
    set_host_stack_limit_enabled(false);

    virtual_bag::State mismatched_state = cross_config_state;
    make_s2_payload(&mismatched_state.items[0][0], 17);
    virtual_bag::State mismatched_parsed{};
    CHECK(virtual_bag::parse_state_json(
        virtual_bag::state_json(mismatched_state).c_str(), &mismatched_parsed,
        host_category_uses_stack_count));
    CHECK_EQ(mismatched_parsed.items[0][0].count, 199);
    CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(
                  virtual_bag::payload_count(mismatched_parsed.items[0][0]))), 199);

    virtual_bag::Item equipment{};
    equipment.category = 333;
    equipment.count = 1;
    make_s2_payload(&equipment, 100);
    virtual_bag::State equipment_state{};
    equipment_state.items[0][0] = equipment;
    const std::string equipment_json = virtual_bag::state_json(equipment_state);
    virtual_bag::State equipment_parsed{};
    CHECK(virtual_bag::parse_state_json(
        equipment_json.c_str(), &equipment_parsed, host_category_uses_stack_count));
    CHECK_EQ(equipment_parsed.items[0][0].count, 1);
    CHECK_EQ(equipment_parsed.items[0][0].payload_size, equipment.payload_size);
    CHECK(std::memcmp(equipment_parsed.items[0][0].payload.data(), equipment.payload.data(),
                      equipment.payload.size()) == 0);

    virtual_bag::State legal_state{};
    legal_state.items[0][0].category = 401;
    legal_state.items[0][0].count = 7;
    make_s2_payload(&legal_state.items[0][0], 7);
    const std::string legal_json = virtual_bag::state_json(legal_state);
    virtual_bag::State legal_parsed{};
    CHECK(virtual_bag::parse_state_json(
        legal_json.c_str(), &legal_parsed, host_category_uses_stack_count));
    CHECK_EQ(legal_parsed.items[0][0].count, 7);
    CHECK(std::memcmp(legal_parsed.items[0][0].payload.data(), legal_state.items[0][0].payload.data(),
                      legal_state.items[0][0].payload.size()) == 0);
}

// 跨配置往返（读档不改写 canonical）：
// 已持久化的 canonical 数量与 payload 数量位在启用/关闭切换与反复读档下
// 原样保留——读档路径与 stack_limit_enabled() 完全解耦，解码永远按 S2，
// 模式只影响写路径 clamp。无版本标识：切换不依赖任何 encodingVersion 状态。
static void test_virtual_bag_cross_config_roundtrip() {
    using namespace virtual_bag;
    const uint32_t counts[] = {99, 127, 128, 217, 999};
    for (const uint32_t count : counts) {
        State saved{};
        saved.items[0][0].category = 401;
        saved.items[0][0].count = static_cast<int>(count);
        make_s2_payload(&saved.items[0][0], count);

        set_host_stack_limit_enabled(true);
        std::string json = state_json(saved);
        std::string previous_payload(
            saved.items[0][0].payload.begin(),
            saved.items[0][0].payload.begin() + saved.items[0][0].payload_size);
        for (int round = 0; round < 3; ++round) {
            // 关闭态读档：不得 clamp、不得按当前配置重编码。
            set_host_stack_limit_enabled(false);
            State parsed_disabled{};
            CHECK(parse_state_json(json.c_str(), &parsed_disabled,
                                   host_category_uses_stack_count));
            CHECK_EQ(parsed_disabled.items[0][0].count, static_cast<int>(count));
            CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(
                          payload_count(parsed_disabled.items[0][0]))),
                     static_cast<int>(count));
            const std::string disabled_payload(
                parsed_disabled.items[0][0].payload.begin(),
                parsed_disabled.items[0][0].payload.begin() +
                    parsed_disabled.items[0][0].payload_size);
            CHECK(disabled_payload == previous_payload);

            // 关闭态再保存 → 启用态读档：仍不得改写。
            const std::string disabled_json = state_json(parsed_disabled);
            set_host_stack_limit_enabled(true);
            State parsed_enabled{};
            CHECK(parse_state_json(disabled_json.c_str(), &parsed_enabled,
                                   host_category_uses_stack_count));
            CHECK_EQ(parsed_enabled.items[0][0].count, static_cast<int>(count));
            CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(
                          payload_count(parsed_enabled.items[0][0]))),
                     static_cast<int>(count));
            const std::string enabled_payload(
                parsed_enabled.items[0][0].payload.begin(),
                parsed_enabled.items[0][0].payload.begin() +
                    parsed_enabled.items[0][0].payload_size);
            CHECK(enabled_payload == previous_payload);
            json = state_json(parsed_enabled);
        }
    }
    set_host_stack_limit_enabled(false);

    // 旧字段宽容忽略：携带 \"encodingVersion\"（任意值，含历史 1/2/未知）的 JSON
    // 不报错、不迁移，descriptor 与 payload 均按统一 S2 语义读取，值不变。
    State legacy_field_state{};
    legacy_field_state.items[0][0].category = 401;
    legacy_field_state.items[0][0].count = 217;
    make_s2_payload(&legacy_field_state.items[0][0], 217);
    std::string legacy_json = state_json(legacy_field_state);
    const std::string legacy_payload(
        legacy_field_state.items[0][0].payload.begin(),
        legacy_field_state.items[0][0].payload.begin() +
            legacy_field_state.items[0][0].payload_size);
    for (const char* injected : {"\"encodingVersion\":1,", "\"encodingVersion\":2,",
                                 "\"encodingVersion\":99,"}) {
        const std::string tagged = "{\"" + std::string(injected) + legacy_json.substr(1);
        State legacy_parsed{};
        CHECK(parse_state_json(tagged.c_str(), &legacy_parsed, host_category_uses_stack_count));
        CHECK_EQ(legacy_parsed.items[0][0].count, 217);
        CHECK_EQ(static_cast<int>(stack_codec::s2_read_count(
                      payload_count(legacy_parsed.items[0][0]))),
                 217);
        const std::string parsed_payload(
            legacy_parsed.items[0][0].payload.begin(),
            legacy_parsed.items[0][0].payload.begin() +
                legacy_parsed.items[0][0].payload_size);
        CHECK(parsed_payload == legacy_payload);
    }
    set_host_stack_limit_enabled(true);
    State legacy_enabled_parsed{};
    CHECK(parse_state_json(legacy_json.c_str(), &legacy_enabled_parsed,
                           host_category_uses_stack_count));
    CHECK_EQ(legacy_enabled_parsed.items[0][0].count, 217);
    set_host_stack_limit_enabled(false);
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
    make_s2_payload(&payload_item, 4);
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
    p.src_bag = virtual_bag::kOriginalTaskBag;
    CHECK_EQ(virtual_bag::recovery_action(state, p), virtual_bag::RecoveryAction::kRollback);
    p2.dst_bag = virtual_bag::kOriginalTaskBag;
    CHECK_EQ(virtual_bag::recovery_action(state, p2), virtual_bag::RecoveryAction::kRollback);
}

static void test_virtual_bag_transaction_domain() {
    using namespace virtual_bag;

    void* expected_item = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
    CHECK(original_to_extension_source_slot_postcondition(expected_item, nullptr));
    CHECK(!original_to_extension_source_slot_postcondition(expected_item, expected_item));
    CHECK(!original_to_extension_source_slot_postcondition(nullptr, nullptr));

    CHECK(valid_original_transaction_bag(0));
    CHECK(valid_original_transaction_bag(4));
    CHECK(!valid_original_transaction_bag(-1));
    CHECK(!valid_original_transaction_bag(kOriginalTaskBag));
    CHECK(!valid_original_transaction_bag(6));
    CHECK(!valid_original_transaction_bag(255));

    CHECK(valid_extension_logical_bag(kExtensionLogicalBagFirst));
    CHECK(valid_extension_logical_bag(kExtensionLogicalBagLast));
    CHECK(!valid_extension_logical_bag(kOriginalTaskBag));
    CHECK(!valid_extension_logical_bag(11));
    CHECK_EQ(extension_internal_bag(kExtensionLogicalBagFirst), 0);
    CHECK_EQ(extension_internal_bag(kExtensionLogicalBagLast), kBagCount - 1);
    CHECK_EQ(extension_internal_bag(11), -1);
    CHECK_EQ(extension_logical_bag(0), kExtensionLogicalBagFirst);
    CHECK_EQ(extension_logical_bag(kBagCount - 1), kExtensionLogicalBagLast);
    CHECK_EQ(extension_logical_bag(kBagCount), -1);

    CHECK(valid_transaction_domain(kTransferOriginalToExtension, 0, 0, 4, 15));
    CHECK(valid_transaction_domain(kTransferExtensionToOriginal, 4, 15, 0, 0));
    CHECK(!valid_transaction_domain(kTransferOriginalToExtension, -1, 0, 0, 0));
    CHECK(!valid_transaction_domain(kTransferOriginalToExtension, kOriginalTaskBag, 0, 0, 0));
    CHECK(!valid_transaction_domain(kTransferExtensionToOriginal, 0, 0, kOriginalTaskBag, 0));
    CHECK(!valid_transaction_domain(kTransferOriginalToExtension, 0, 0, kBagCount, 0));
    CHECK(!valid_transaction_domain(kTransferExtensionToOriginal, kBagCount, 0, 0, 0));
    CHECK(!valid_transaction_domain(kTransferOriginalToExtension, 0, -1, 0, 0));
    CHECK(!valid_transaction_domain(kTransferExtensionToOriginal, 0, 0, 0, kSlotCount));

    Item item{};
    item.category = 401;
    item.count = 4;
    make_s2_payload(&item, item.count);

    State source{};
    source.types = {4, 0, 0, 0, 0};
    normalize(&source);
    source.items[0][0] = item;
    source.pending.valid = true;
    source.pending.direction = kTransferOriginalToExtension;
    source.pending.src_bag = 0;
    source.pending.src_slot = 0;
    source.pending.dst_bag = 0;
    source.pending.dst_slot = 1;
    source.pending.payload_size = item.payload_size;
    source.pending.payload = item.payload;

    std::string invalid_pending_json = state_json(source);
    const std::string valid_source_bag = "\"srcBag\":0";
    const size_t source_bag_pos = invalid_pending_json.find(valid_source_bag);
    CHECK(source_bag_pos != std::string::npos);
    invalid_pending_json.replace(source_bag_pos, valid_source_bag.size(),
                                 "\"srcBag\":5");

    PendingTransfer rejected{};
    CHECK(parse_pending_transfer_result(invalid_pending_json.c_str(), &rejected) ==
          PendingTransferParseResult::kInvalidTransactionDomain);
    CHECK_EQ(static_cast<int>(rejected.src_bag), kOriginalTaskBag);
    CHECK_EQ(static_cast<int>(rejected.payload_size),
             static_cast<int>(source.pending.payload_size));
    CHECK(rejected.payload == source.pending.payload);
    CHECK_EQ(std::string(rejected.transaction_id),
             std::string(source.pending.transaction_id));

    State parsed{};
    CHECK(parse_state_json(invalid_pending_json.c_str(), &parsed));
    CHECK_EQ(parsed.items[0][0].category, item.category);
    CHECK_EQ(parsed.items[0][0].count, item.count);
    CHECK(!parsed.pending.valid);

    std::string negative_pending_json = state_json(source);
    const size_t negative_source_bag_pos = negative_pending_json.find(valid_source_bag);
    CHECK(negative_source_bag_pos != std::string::npos);
    negative_pending_json.replace(negative_source_bag_pos, valid_source_bag.size(),
                                  "\"srcBag\":-1");
    CHECK(parse_pending_transfer_result(negative_pending_json.c_str(), &rejected) ==
          PendingTransferParseResult::kMalformed);
    State negative_parsed{};
    CHECK(parse_state_json(negative_pending_json.c_str(), &negative_parsed));
    CHECK_EQ(negative_parsed.items[0][0].category, item.category);
    CHECK_EQ(negative_parsed.items[0][0].count, item.count);
    CHECK(!negative_parsed.pending.valid);
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

// save-backup 纯逻辑层（feature/save_backup/save_backup_bundle.cpp，编译真实被测源文件）：

static void test_save_backup_digests() {
    namespace sb = save_backup;
    using sb::Entry; using sb::ParsedBundle;
    // SHA-256（FIPS 180-4 标准向量）
    CHECK_EQ(sb::sha256_hex({}, {}), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    {
        std::vector<uint8_t> abc = {'a', 'b', 'c'};
        CHECK_EQ(sb::sha256_hex(abc, {}),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        // 两段拼接等价于连接后单段。
        std::vector<uint8_t> second = {'d', 'e'};
        CHECK_EQ(sb::sha256_hex(abc, second), sb::sha256_hex({'a', 'b', 'c', 'd', 'e'}, {}));
    }
    // MD5（RFC 1321 标准向量）
    CHECK_EQ(sb::md5_hex(reinterpret_cast<const uint8_t*>(""), 0),
             std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(sb::md5_hex(reinterpret_cast<const uint8_t*>("abc"), 3),
             std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(sb::md5_hex(reinterpret_cast<const uint8_t*>("message digest"), 14),
             std::string("f96b697d7cb7938d525a2f31aaf161d0"));
    // CRC32（IEEE，与 java.util.zip.CRC32 一致）
    CHECK_EQ(sb::crc32_ieee(reinterpret_cast<const uint8_t*>(""), 0), 0x00000000u);
    CHECK_EQ(sb::crc32_ieee(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
}

static void test_save_backup_base64() {
    namespace sb = save_backup;
    using sb::Entry; using sb::ParsedBundle;
    const uint8_t hello[5] = {'H', 'e', 'l', 'l', 'o'};
    CHECK_EQ(sb::base64_encode(hello, 5), std::string("SGVsbG8="));
    std::vector<uint8_t> dec;
    CHECK(sb::base64_decode("SGVsbG8=", dec));
    CHECK_EQ(dec, (std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'}));
    // 任意长度往返（含 padding 边界）
    std::vector<uint8_t> data;
    for (int i = 1; i <= 255; ++i) data.push_back(static_cast<uint8_t>(i * 7 + 1));
    for (size_t len : {size_t(1), size_t(2), size_t(3), size_t(19), size_t(255)}) {
        const std::string enc = sb::base64_encode(data.data(), len);
        CHECK(sb::base64_decode(enc, dec));
        CHECK_EQ(dec.size(), len);
        CHECK(std::memcmp(dec.data(), data.data(), len) == 0);
    }
    // 空输入
    CHECK(sb::base64_decode("", dec));
    CHECK(dec.empty());
    // 非法输入拒绝
    CHECK(!sb::base64_decode("A", dec));
    CHECK(!sb::base64_decode("AA=A", dec));
    CHECK(!sb::base64_decode("!!!!", dec));
}

static void test_save_backup_bundle_roundtrip() {
    namespace sb = save_backup;
    using sb::Entry; using sb::ParsedBundle;
    const std::vector<uint8_t> plain = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<uint8_t> module = {0x4D, 0x53, 0x41, 0x56, 0, 1, 0, 9};
    const std::string meta = "{\"source_slot\":1,\"export_time\":1700000000000,\"map_id\":37,"
                             "\"hero_level\":5,\"hero_index\":0,\"save_version\":5,"
                             "\"save_time\":1699999999000,\"original_sha256\":\"ab12\","
                             "\"module_sha256\":\"cd34\",\"checksum\":\"89abcdef0123\"}";
    std::vector<uint8_t> bundle;
    sb::build_bundle(1, 1700000000000LL, plain, module, {}, meta, bundle);
    // v2 比 v1 多 u32 warehouseLen（空 blob 时 blob 为 0 字节）。
    CHECK_EQ(bundle.size(),
             sb::kMinBundleBytes + 4 + plain.size() + module.size() + meta.size());

    sb::ParsedBundle parsed;
    CHECK(sb::parse_bundle(bundle.data(), bundle.size(), parsed));
    CHECK_EQ(parsed.source_slot, 1);
    CHECK_EQ(parsed.export_time_ms, 1700000000000LL);
    CHECK(parsed.orig_plain == plain);
    CHECK(parsed.module == module);
    CHECK_EQ(parsed.meta_json, meta);
    CHECK_EQ(parsed.size_bytes, static_cast<long long>(bundle.size()));

    // 空 module（仅原版备份）往返
    std::vector<uint8_t> bundle2;
    sb::build_bundle(2, 42, plain, {}, {}, meta, bundle2);
    sb::ParsedBundle parsed2;
    CHECK(sb::parse_bundle(bundle2.data(), bundle2.size(), parsed2));
    CHECK(parsed2.module.empty());

    // 损坏拒绝：CRC 篡改、magic 篡改、版本不识别、分段越界、尺寸过小
    auto corrupted = bundle;
    corrupted[corrupted.size() - 1] ^= 1;
    CHECK(!sb::parse_bundle(corrupted.data(), corrupted.size(), parsed));
    corrupted = bundle;
    corrupted[0] = 'X';
    CHECK(!sb::parse_bundle(corrupted.data(), corrupted.size(), parsed));
    corrupted = bundle;
    corrupted[4] = 9;  // version 高字节
    CHECK(!sb::parse_bundle(corrupted.data(), corrupted.size(), parsed));
    corrupted = bundle;
    corrupted[6] = 3;  // sourceSlot > 2
    CHECK(!sb::parse_bundle(corrupted.data(), corrupted.size(), parsed));
    corrupted = bundle;
    corrupted[18] = 0xFF;  // origLen 高字节越界
    CHECK(!sb::parse_bundle(corrupted.data(), corrupted.size(), parsed));
    CHECK(!sb::parse_bundle(bundle.data(), sb::kMinBundleBytes - 1, parsed));
}

static void test_save_backup_meta_entry() {
    namespace sb = save_backup;
    using sb::Entry; using sb::ParsedBundle;
    const std::vector<uint8_t> plain(4, 0xAB);
    const std::string meta = sb::build_meta_json(1, 1700000000000LL, 37, 5, 0, 5, 1699999999000LL,
                                             "aa00bb00bb00bb00bb00bb00bb00bb00bb00bb00bb00bb00bb00bb00bb00bb00",
                                             "cc00cc00cc00cc00cc00cc00cc00cc00cc00cc00cc00cc00cc00cc00cc00cc00",
                                             "89abcdef0123");
    std::vector<uint8_t> bundle;
    sb::build_bundle(1, 1700000000000LL, plain, {}, {}, meta, bundle);
    sb::ParsedBundle parsed;
    CHECK(sb::parse_bundle(bundle.data(), bundle.size(), parsed));
    Entry e;
    sb::entry_from_bundle("20260912-122823_s1_89abcdef0123.qol_save", parsed, e);
    CHECK_EQ(e.file_name, std::string("20260912-122823_s1_89abcdef0123.qol_save"));
    CHECK_EQ(e.size_bytes, static_cast<long long>(bundle.size()));
    CHECK_EQ(e.source_slot, 1);
    CHECK_EQ(e.export_time_ms, 1700000000000LL);
    CHECK_EQ(e.map_id, 37);
    CHECK_EQ(e.hero_level, 5);
    CHECK_EQ(e.hero_index, 0);
    CHECK_EQ(e.save_version, 5);
    CHECK_EQ(e.save_time, 1699999999000LL);
    CHECK(e.original_sha256.length() == 64);
    CHECK_EQ(e.checksum, std::string("89abcdef0123"));

    // metaJson 权威：meta 内 checksum 与文件名后缀不同时取 meta。
    Entry e2;
    sb::entry_from_bundle("20260912-122823_s1_ffffffffffff.qol_save", parsed, e2);
    CHECK_EQ(e2.checksum, std::string("89abcdef0123"));

    // checksum 缺失 → 文件名兜底；字段缺失 → 默认值（map_id=0、hero=-1、version=0）。
    ParsedBundle legacy;
    legacy.source_slot = 2;
    legacy.export_time_ms = 7;
    legacy.size_bytes = 100;
    legacy.meta_json = "{}";
    Entry e3;
    sb::entry_from_bundle("20260912-000000_s2_deadbeefcafe.qol_save", legacy, e3);
    CHECK_EQ(e3.checksum, std::string("deadbeefcafe"));
    CHECK_EQ(e3.map_id, 0);
    CHECK_EQ(e3.hero_level, -1);
    CHECK_EQ(e3.hero_index, -1);
    CHECK_EQ(e3.save_version, 0);
    CHECK_EQ(e3.save_time, 0);
    CHECK(e3.original_sha256.empty());
    CHECK(e3.module_sha256.empty());

    // entry_json 字段名与顺序（HTTP BackupMeta 契约）。
    const std::string j = sb::entry_json(e3);
    CHECK(j.find("\"file_name\":") != std::string::npos);
    CHECK(j.find("\"size_bytes\":100") != std::string::npos);
    CHECK(j.find("\"source_slot\":2") != std::string::npos);
    CHECK(j.find("\"export_time\":7") != std::string::npos);
    CHECK(j.find("\"map_id\":0") != std::string::npos);
    CHECK(j.find("\"hero_level\":-1") != std::string::npos);
    CHECK(j.find("\"hero_index\":-1") != std::string::npos);
    CHECK(j.find("\"save_version\":0") != std::string::npos);
    CHECK(j.find("\"save_time\":0") != std::string::npos);
    CHECK(j.find("\"original_sha256\":\"\"") != std::string::npos);
    CHECK(j.find("\"module_sha256\":\"\"") != std::string::npos);
    CHECK(j.find("\"checksum\":\"deadbeefcafe\"") != std::string::npos);

    // 极简提取器：键完整匹配（save_time 不串到 export_time）、缺省返回 false。
    long long v = 0;
    std::string s;
    CHECK(sb::json_find_int(meta, "save_time", v));
    CHECK_EQ(v, 1699999999000LL);
    CHECK(sb::json_find_int(meta, "map_id", v));
    CHECK_EQ(v, 37);
    CHECK(!sb::json_find_int(meta, "missing", v));
    CHECK(sb::json_find_string(meta, "checksum", s));
    CHECK_EQ(s, std::string("89abcdef0123"));
    CHECK(!sb::json_find_string(meta, "missing", s));
}

// entry_json 携带 map_name 字段（v0.7.x 给游戏内备份面板显示地图名）。
// Entry.map_name 缺省空串、entry_json 输出空 map_name 字段不丢失。
static void test_save_backup_entry_map_name() {
    namespace sb = save_backup;
    sb::Entry e;
    e.file_name = "20260912-130000_s0_89abcdef0123.qol_save";
    e.size_bytes = 12345;
    e.source_slot = 0;
    e.export_time_ms = 1700000000000LL;
    e.map_id = 37;
    e.map_name = "影子丛林1";  // Kotlin 启动期下发的中文地图名
    e.hero_level = 5;
    e.checksum = "89abcdef0123";
    const std::string j = sb::entry_json(e);
    // map_name 必须在 JSON 内出现且等于原值
    CHECK(j.find("\"map_name\":\"影子丛林1\"") != std::string::npos);
    CHECK(j.find("\"map_id\":37") != std::string::npos);

    // 缺省空串：entry_json 仍输出 "map_name":"" 字段
    sb::Entry empty;
    empty.file_name = "x.qol_save";
    empty.size_bytes = 0;
    empty.checksum = "000000000000";
    const std::string j2 = sb::entry_json(empty);
    CHECK(j2.find("\"map_name\":\"\"") != std::string::npos);

    // entry_from_bundle 不填充 map_name（Kotlin 启动期设置由 native 层注入；
    // bundle 字节布局未引入新字段，旧 .qol_save 解析仍向前兼容）。
    const std::vector<uint8_t> plain(4, 0xAB);
    const std::string meta = sb::build_meta_json(0, 1, 30, 5, 0, 5, 0,
                                                 std::string(64, 'a'), std::string(),
                                                 "89abcdef0123");
    std::vector<uint8_t> bundle;
    sb::build_bundle(0, 1, plain, {}, {}, meta, bundle);
    sb::ParsedBundle parsed;
    CHECK(sb::parse_bundle(bundle.data(), bundle.size(), parsed));
    sb::Entry from_bundle;
    sb::entry_from_bundle("x_s0_89abcdef0123.qol_save", parsed, from_bundle);
    CHECK_EQ(from_bundle.map_id, 30);
    CHECK(from_bundle.map_name.empty());  // bundle 解析不携带 map_name（native 内存表查表）
}

static void test_save_backup_checksum_name() {
    namespace sb = save_backup;
    using sb::Entry; using sb::ParsedBundle;
    CHECK(sb::valid_checksum("89abcdef0123"));
    CHECK(sb::valid_checksum("000000000000"));
    CHECK(!sb::valid_checksum("89ABCDEF0123"));  // 大写拒绝
    CHECK(!sb::valid_checksum("89abcdef01"));    // 10 位
    CHECK(!sb::valid_checksum("89abcdef01234")); // 14 位
    CHECK(!sb::valid_checksum("89abcdefg123"));  // 非 hex
    std::string out;
    CHECK(sb::extract_checksum_from_name("20260912-122823_s0_89abcdef0123.qol_save", out));
    CHECK_EQ(out, std::string("89abcdef0123"));
    CHECK(!sb::extract_checksum_from_name("20260912-122823_s0_89abcdef0123.txt", out));
    CHECK(!sb::extract_checksum_from_name("89abcdef0123.qol_save", out));  // 缺 `_` 前缀
    CHECK(!sb::extract_checksum_from_name("short.qol_save", out));
}

static void test_save_backup_module_container() {
    namespace sb = save_backup;
    using sb::Entry; using sb::ParsedBundle;
    // 构造 fake MSAV 容器：magic + ver + slot + generation + count + 6 字节负载 + crc。
    std::vector<uint8_t> container;
    container.push_back(0x4D); container.push_back(0x53); container.push_back(0x41); container.push_back(0x56);
    container.push_back(0x00); container.push_back(0x01);
    container.push_back(0x00);  // slot=0
    for (int i = 0; i < 8; ++i) container.push_back(0);
    container.push_back(0x00); container.push_back(0x01);
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    container.insert(container.end(), payload, payload + sizeof(payload));
    const uint32_t crc = sb::crc32_ieee(container.data(), container.size());
    container.push_back(static_cast<uint8_t>(crc >> 24));
    container.push_back(static_cast<uint8_t>(crc >> 16));
    container.push_back(static_cast<uint8_t>(crc >> 8));
    container.push_back(static_cast<uint8_t>(crc));
    CHECK(sb::module_container_valid(container));

    // 重定槽：第 6 字节改目标槽 + CRC 重算后仍校验通过，其余字节不变。
    std::vector<uint8_t> reslotted = container;
    CHECK(sb::module_container_reslot(reslotted, 2));
    CHECK_EQ(static_cast<int>(reslotted[6]), 2);
    CHECK(sb::module_container_valid(reslotted));
    CHECK(std::memcmp(reslotted.data(), container.data(), 6) == 0);
    // 第 6 字节之后、尾部 CRC 之前的字节不变。
    CHECK(std::memcmp(reslotted.data() + 7, container.data() + 7, container.size() - 11) == 0);

    // 损坏拒绝：翻转 payload 一字节 → CRC 失配。
    std::vector<uint8_t> broken = container;
    broken[15] ^= 1;
    CHECK(!sb::module_container_valid(broken));
    CHECK(!sb::module_container_reslot(broken, 1));
    // 过小/坏 magic 拒绝。
    std::vector<uint8_t> tiny = {0x4D, 0x53, 0x41};
    CHECK(!sb::module_container_valid(tiny));
    std::vector<uint8_t> bad_magic = container;
    bad_magic[0] = 'X';
    CHECK(!sb::module_container_valid(bad_magic));
}

// 个人仓库段（v2 bundle）：encode/decode 往返、build/parse 携带、v1 兼容、
// 解码 fail-closed、以及「空仓库不改变旧 checksum」回归。
static void test_save_backup_warehouse() {
    namespace sb = save_backup;
    using sb::WarehouseFile;

    // 1) 空列表编码为空 blob；空 blob/空指针解码为空且成功。
    CHECK(sb::encode_warehouse({}).empty());
    std::vector<WarehouseFile> empty_decoded;
    CHECK(sb::decode_warehouse(nullptr, 0, empty_decoded));
    CHECK(empty_decoded.empty());

    // 2) 两个 WarehouseFile（主文件 + .bak）encode/decode 往返一致。
    WarehouseFile main_file;
    main_file.suffix = ".wh4-000001a043a2bba1";
    main_file.data = {0x51, 0x53, 0x42, 0x31, 0x00, 0x01, 0x02, 0x03};
    WarehouseFile bak_file;
    bak_file.suffix = ".wh4-000001a043a2bba1.bak";
    bak_file.data = {0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<WarehouseFile> files = {main_file, bak_file};
    const std::vector<uint8_t> blob = sb::encode_warehouse(files);
    CHECK(!blob.empty());
    std::vector<WarehouseFile> decoded;
    CHECK(sb::decode_warehouse(blob.data(), blob.size(), decoded));
    CHECK_EQ(decoded.size(), static_cast<size_t>(2));
    CHECK_EQ(decoded[0].suffix, main_file.suffix);
    CHECK(decoded[0].data == main_file.data);
    CHECK_EQ(decoded[1].suffix, bak_file.suffix);
    CHECK(decoded[1].data == bak_file.data);

    // 3) build_bundle 携带 blob → parse_bundle：has_warehouse 为真且内容一致。
    const std::vector<uint8_t> plain = {1, 2, 3, 4};
    const std::string meta = "{}";
    std::vector<uint8_t> bundle;
    sb::build_bundle(1, 123, plain, {}, blob, meta, bundle);
    sb::ParsedBundle parsed;
    CHECK(sb::parse_bundle(bundle.data(), bundle.size(), parsed));
    CHECK(parsed.has_warehouse);
    CHECK_EQ(parsed.warehouse.size(), static_cast<size_t>(2));
    CHECK_EQ(parsed.warehouse[0].suffix, main_file.suffix);
    CHECK(parsed.warehouse[0].data == main_file.data);
    CHECK_EQ(parsed.warehouse[1].suffix, bak_file.suffix);
    CHECK(parsed.warehouse[1].data == bak_file.data);

    // 4) 手写 v1 bundle（无仓库段）：parse 成功且 has_warehouse=false。
    auto wb16 = [](std::vector<uint8_t>& o, uint16_t v) {
        o.push_back(static_cast<uint8_t>(v >> 8));
        o.push_back(static_cast<uint8_t>(v));
    };
    auto wb32 = [](std::vector<uint8_t>& o, uint32_t v) {
        o.push_back(static_cast<uint8_t>(v >> 24));
        o.push_back(static_cast<uint8_t>(v >> 16));
        o.push_back(static_cast<uint8_t>(v >> 8));
        o.push_back(static_cast<uint8_t>(v));
    };
    auto wb64 = [](std::vector<uint8_t>& o, uint64_t v) {
        for (int i = 7; i >= 0; --i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    const std::vector<uint8_t> v1_plain = {9, 8, 7};
    const std::vector<uint8_t> v1_module = {0x4D};
    const std::string v1_meta = "{\"checksum\":\"001122334455\"}";
    std::vector<uint8_t> v1;
    wb32(v1, sb::kBundleMagic);
    wb16(v1, sb::kBundleVersionV1);
    v1.push_back(1);
    v1.push_back(0);
    v1.push_back(0);
    v1.push_back(0);
    wb64(v1, 42);
    wb32(v1, static_cast<uint32_t>(v1_plain.size()));
    v1.insert(v1.end(), v1_plain.begin(), v1_plain.end());
    wb32(v1, static_cast<uint32_t>(v1_module.size()));
    v1.insert(v1.end(), v1_module.begin(), v1_module.end());
    wb16(v1, static_cast<uint16_t>(v1_meta.size()));
    v1.insert(v1.end(), v1_meta.begin(), v1_meta.end());
    wb32(v1, sb::crc32_ieee(v1.data(), v1.size()));
    sb::ParsedBundle parsed_v1;
    CHECK(sb::parse_bundle(v1.data(), v1.size(), parsed_v1));
    CHECK(!parsed_v1.has_warehouse);
    CHECK(parsed_v1.warehouse.empty());
    CHECK(parsed_v1.orig_plain == v1_plain);
    CHECK(parsed_v1.module == v1_module);
    CHECK_EQ(parsed_v1.meta_json, v1_meta);

    // 5) decode fail-closed：截断、count 越界、后缀非法、dataLen 越界。
    std::vector<WarehouseFile> bad;
    CHECK(!sb::decode_warehouse(blob.data(), 1, bad));               // 长度 < 2
    CHECK(!sb::decode_warehouse(blob.data(), blob.size() - 1, bad));  // 尾部截断
    std::vector<uint8_t> count_bad = blob;
    count_bad[0] = 0x00;
    count_bad[1] = 0x41;  // count=65 > kMaxWarehouseFiles
    CHECK(!sb::decode_warehouse(count_bad.data(), count_bad.size(), bad));

    // 单项辅助：suffix 指定、data 指定；构造合法 count=1 头。
    auto make_one = [](const std::string& suffix, uint32_t data_len,
                       const std::vector<uint8_t>& body) {
        std::vector<uint8_t> one;
        one.push_back(0x00);
        one.push_back(0x01);
        one.push_back(static_cast<uint8_t>(suffix.size() >> 8));
        one.push_back(static_cast<uint8_t>(suffix.size()));
        one.insert(one.end(), suffix.begin(), suffix.end());
        one.push_back(static_cast<uint8_t>(data_len >> 24));
        one.push_back(static_cast<uint8_t>(data_len >> 16));
        one.push_back(static_cast<uint8_t>(data_len >> 8));
        one.push_back(static_cast<uint8_t>(data_len));
        one.insert(one.end(), body.begin(), body.end());
        return one;
    };
    // 后缀非 `.wh4-`。
    {
        const std::vector<uint8_t> one = make_one(".evil", 1, {0x00});
        CHECK(!sb::decode_warehouse(one.data(), one.size(), bad));
    }
    // 后缀含 `..`。
    {
        const std::vector<uint8_t> one = make_one(".wh4-..", 1, {0x00});
        CHECK(!sb::decode_warehouse(one.data(), one.size(), bad));
    }
    // dataLen 声明越界（0xFFFFFFFF > kMaxWarehouseDataBytes）。
    {
        const std::vector<uint8_t> one = make_one(main_file.suffix, 0xFFFFFFFFu, {});
        CHECK(!sb::decode_warehouse(one.data(), one.size(), bad));
    }
    // 后缀合法但 dataLen 超过实际剩余字节。
    {
        const std::vector<uint8_t> one = make_one(main_file.suffix, 8, {0x01, 0x02});
        CHECK(!sb::decode_warehouse(one.data(), one.size(), bad));
    }
    // valid_warehouse_suffix 直接断言。
    CHECK(sb::valid_warehouse_suffix(".wh4-000001a043a2bba1"));
    CHECK(sb::valid_warehouse_suffix(".wh4-000001a043a2bba1.bak"));
    CHECK(!sb::valid_warehouse_suffix("wh4-000001a043a2bba1"));   // 缺前导点
    CHECK(!sb::valid_warehouse_suffix(".wh4-000001a043a2bba1/"));  // 含分隔符
    CHECK(!sb::valid_warehouse_suffix(".wh4-.."));                 // 含 ..
    CHECK(!sb::valid_warehouse_suffix(".wh4"));                    // 过短

    // 6) checksum 回归保护：空 blob 时「module 后接空 blob」与「module」摘要一致。
    const std::vector<uint8_t> module = {0x4D, 0x53, 0x41, 0x56, 1, 2, 3};
    const std::vector<uint8_t> empty_blob = sb::encode_warehouse({});
    std::vector<uint8_t> module_and_empty = module;
    module_and_empty.insert(module_and_empty.end(), empty_blob.begin(), empty_blob.end());
    CHECK_EQ(sb::sha256_hex(plain, module), sb::sha256_hex(plain, module_and_empty));
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
        journal.payload_size = kPayloadHeaderSize;
        journal.payload[0] = static_cast<uint8_t>(kPayloadHeaderSize - 1);
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
        journal.src_bag = kOriginalTaskBag;
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
        journal.direction = kTransferExtensionToOriginal;
        journal.src_bag = 0;
        journal.dst_bag = kOriginalTaskBag;
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
        JournalRecord corrupted{};
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
        std::string task_bag_journal = encoded;
        const std::string valid_source_bag = "\"srcBag\":0";
        const size_t source_bag_pos = task_bag_journal.find(valid_source_bag);
        CHECK(source_bag_pos != std::string::npos);
        task_bag_journal.replace(source_bag_pos, valid_source_bag.size(), "\"srcBag\":5");
        CHECK(!parse_journal_json(task_bag_journal.c_str(), &corrupted));
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

    {
        const char* with_unknown_fields =
            "{\"gameIdentity\":\"aabbccdd00112233\",\"futureField\":{\"x\":1},"
            "\"mode\":\"module\",\"originalSelected\":1,\"types\":[2,0,0,0,0],"
            "\"selected\":0,\"inspected\":-1,\"items\":[" ;
        std::string json = with_unknown_fields;
        for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
            json += '[';
            for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
                if (slot > 0) json += ',';
                json += (bag == 0 && slot == 0) ? R"({"category":7,"count":5})" : "{\"category\":0,\"count\":0}";
            }
            json += ']';
            if (bag + 1 < virtual_bag::kBagCount) json += ',';
        }
        json += "]}";
        virtual_bag::State forward{};
        CHECK(virtual_bag::parse_state_json(json.c_str(), &forward));
        CHECK_EQ(forward.items[0][0].category, 7);
        CHECK_EQ(forward.items[0][0].count, 5);
        CHECK_EQ(forward.selected, 0);
    }

    {
        // Android org.json 把 base64 里的 '/' 序列化成 `\/`；native 解析必须反转义后再解码，
        // 否则带 '/' 的 payload 会被判长度不足、整个 sidecar 回退为空。
        const char* escaped_payload = "Erj\\/CQ8AAAAASA4AZIAkAAAAAA==";
        std::string json =
            "{\"mode\":\"original\",\"originalSelected\":1,\"types\":[0,0,0,0,0],"
            "\"selected\":-1,\"inspected\":-1,\"items\":[";
        for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
            json += '[';
            for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
                if (slot > 0) json += ',';
                if (bag == 0 && slot == 0) {
                    json += "{\"category\":7,\"count\":5,\"payload\":\"";
                    json += escaped_payload;
                    json += "\"}";
                } else {
                    json += "{\"category\":0,\"count\":0}";
                }
            }
            json += ']';
            if (bag + 1 < virtual_bag::kBagCount) json += ',';
        }
        json += "]}";
        virtual_bag::State escaped{};
        CHECK(virtual_bag::parse_state_json(json.c_str(), &escaped));
        CHECK_EQ(escaped.items[0][0].category, 7);
        CHECK_EQ(static_cast<int>(escaped.items[0][0].payload_size), 19);
        uint8_t expected[19];
        const int expected_len = virtual_bag::base64_decode(
            "Erj/CQ8AAAAASA4AZIAkAAAAAA==", 28, expected, sizeof(expected));
        CHECK_EQ(expected_len, 19);
        CHECK(std::memcmp(escaped.items[0][0].payload.data(), expected, 19) == 0);
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
        CHECK_EQ(report.live_handles, 0u);  // 交接槽已回收
    }

    uint32_t c = 0;
    CHECK(ownership::allocate(&ledger, &c) == ownership::Outcome::kOk);
    CHECK(c != b);
    CHECK(ownership::live_state(ledger, c, ownership::State::kModuleOwned));

    CHECK(ownership::allocate(nullptr, &c) == ownership::Outcome::kRejectInvalidState);
    CHECK(ownership::allocate(&ledger, nullptr) == ownership::Outcome::kRejectInvalidState);
    CHECK(ownership::release(&ledger, 0x7FFFFFFF) == ownership::Outcome::kRejectUnknownHandle);
}

static void test_ownership_ledger_p43() {
    // 过期 generation handle：释放后同槽再分配，旧句柄全部操作必须被拒。
    ownership::Ledger ledger{};
    uint32_t a = 0;
    CHECK(ownership::allocate(&ledger, &a) == ownership::Outcome::kOk);
    CHECK(ownership::release(&ledger, a) == ownership::Outcome::kOk);
    uint32_t b = 0;
    CHECK(ownership::allocate(&ledger, &b) == ownership::Outcome::kOk);
    CHECK(b != a);
    CHECK(ownership::handle_slot(b) == ownership::handle_slot(a));
    CHECK(ownership::live_state(ledger, a, ownership::State::kModuleOwned) == false);
    CHECK(ownership::release(&ledger, a) == ownership::Outcome::kRejectUnknownHandle);
    CHECK(ownership::handover_to_inventory(&ledger, a) == ownership::Outcome::kRejectUnknownHandle);
    CHECK(ownership::borrow_for_view(&ledger, a) == ownership::Outcome::kRejectUnknownHandle);
    CHECK(ownership::return_from_view(&ledger, a) == ownership::Outcome::kRejectUnknownHandle);
    CHECK(ownership::live_state(ledger, b, ownership::State::kModuleOwned));

    // 池耗尽与槽位复用：33 次分配被拒；释放后可复用且 generation 递增。
    ownership::Ledger pool{};
    uint32_t handles[ownership::kLedgerCapacity] = {};
    for (int i = 0; i < ownership::kLedgerCapacity; ++i) {
        CHECK(ownership::allocate(&pool, &handles[i]) == ownership::Outcome::kOk);
    }
    uint32_t extra = 0;
    CHECK(ownership::allocate(&pool, &extra) == ownership::Outcome::kRejectPoolExhausted);
    CHECK_EQ(extra, 0u);
    CHECK(ownership::release(&pool, handles[7]) == ownership::Outcome::kOk);
    CHECK(ownership::allocate(&pool, &extra) == ownership::Outcome::kOk);
    CHECK(ownership::handle_slot(extra) == ownership::handle_slot(handles[7]));
    CHECK(extra != handles[7]);
    for (int i = 0; i < ownership::kLedgerCapacity; ++i) {
        if (i == 7) continue;
        CHECK(ownership::release(&pool, handles[i]) == ownership::Outcome::kOk);
    }
    CHECK(ownership::release(&pool, extra) == ownership::Outcome::kOk);
    CHECK_EQ(ownership::audit(pool).outstanding_objects, 0u);
    CHECK(ownership::audit(pool).balanced);

    // 混合生命周期审计：借出中禁止 handover；归还→handover→槽位回收（累计计数）。
    ownership::Ledger mixed{};
    uint32_t h1 = 0;
    uint32_t h2 = 0;
    uint32_t h3 = 0;
    CHECK(ownership::allocate(&mixed, &h1) == ownership::Outcome::kOk);
    CHECK(ownership::allocate(&mixed, &h2) == ownership::Outcome::kOk);
    CHECK(ownership::allocate(&mixed, &h3) == ownership::Outcome::kOk);
    CHECK(ownership::borrow_for_view(&mixed, h2) == ownership::Outcome::kOk);
    CHECK(ownership::handover_to_inventory(&mixed, h2) == ownership::Outcome::kRejectUnknownHandle);
    CHECK(ownership::release(&mixed, h2) == ownership::Outcome::kRejectUnknownHandle);
    CHECK(ownership::return_from_view(&mixed, h2) == ownership::Outcome::kOk);
    CHECK(ownership::handover_to_inventory(&mixed, h2) == ownership::Outcome::kOk);
    CHECK(ownership::release(&mixed, h1) == ownership::Outcome::kOk);
    {
        const ownership::Audit report = ownership::audit(mixed);
        CHECK(report.balanced);
        CHECK_EQ(report.outstanding_borrows, 0u);
        CHECK_EQ(report.outstanding_objects, 1u);
        CHECK_EQ(report.inventory_owned, 1u);
        CHECK_EQ(mixed.total_allocated, mixed.total_released + mixed.total_handed_over + 1u);
    }
    CHECK(ownership::release(&mixed, h3) == ownership::Outcome::kOk);
    CHECK_EQ(ownership::audit(mixed).live_handles, 0u);  // h2 交接槽已回收
}

static void test_unequip_bag() {
    virtual_bag::State state{};
    CHECK(!virtual_bag::unequip_bag(&state, -1));
    CHECK(!virtual_bag::unequip_bag(&state, 5));
    CHECK(virtual_bag::set_test_equipped(&state, 0, 2));
    CHECK_EQ(virtual_bag::unequip_bag(&state, 0), true);
    CHECK_EQ((int)state.types[0], 0);
    CHECK_EQ((int)state.capacities[0], 0);
    CHECK_EQ(state.selected, -1);

    CHECK(virtual_bag::set_test_equipped(&state, 1, 3));
    CHECK_EQ(virtual_bag::click(&state, 1), virtual_bag::ClickResult::kSelected);
    CHECK_EQ(virtual_bag::click(&state, 1), virtual_bag::ClickResult::kInspected);
    CHECK_EQ(state.info_bag, 1);
    CHECK(virtual_bag::set_item(&state, 1, 0, 7, 3));
    CHECK_EQ(virtual_bag::unequip_bag(&state, 1), false);
    CHECK_EQ((int)state.capacities[1], 12);
    CHECK_EQ(state.info_bag, 1);
    state.items[1][0] = {};
    CHECK_EQ(virtual_bag::unequip_bag(&state, 1), true);
    CHECK_EQ((int)state.capacities[1], 0);
    CHECK_EQ(state.info_bag, -1);
    CHECK_EQ(state.mode, virtual_bag::Mode::kOriginal);
}

static void test_equip_bag() {
    virtual_bag::State state{};
    CHECK(!virtual_bag::equip_bag(&state, -1, 2));
    CHECK(!virtual_bag::equip_bag(&state, 0, 0));
    CHECK(!virtual_bag::equip_bag(&state, 0, 5));
    CHECK(virtual_bag::equip_bag(&state, 0, 4));
    CHECK_EQ((int)state.types[0], 4);
    CHECK_EQ((int)state.capacities[0], 16);
    CHECK(!virtual_bag::equip_bag(&state, 0, 1));  // 已占用位拒绝且不改类型
    CHECK_EQ((int)state.types[0], 4);
    CHECK_EQ((int)state.capacities[0], 16);
    CHECK(virtual_bag::equip_bag(&state, 2, 1));
    CHECK_EQ((int)state.types[2], 1);
    CHECK_EQ((int)state.capacities[2], 4);
    CHECK_EQ((int)state.capacities[1], 0);
}

// P4.4 五态事务纯模型：域校验、pending 构造与 journal v1 映射、
// 逐阶段失败注入（logical/original/committed 边界）的还原断言。
static bool p44_item_identity(const virtual_bag::Item& a, const virtual_bag::Item& b) {
    return a.category == b.category && a.count == b.count &&
           a.payload_size == b.payload_size &&
           std::memcmp(a.payload.data(), b.payload.data(), virtual_bag::kSerializedItemBuffer) == 0;
}

static void test_p44_transaction_stages() {
    using namespace virtual_bag;

    // pending 域接受 ext→ext（内部袋索引）；journal v1 域（不建 journal）仍拒绝。
    CHECK(valid_pending_transaction_domain(kTransferExtensionToExtension, 2, 0, 3, 4));
    CHECK(valid_pending_transaction_domain(kTransferExtensionToExtension, 2, 0, 2, 1));
    CHECK(!valid_pending_transaction_domain(kTransferExtensionToExtension, 2, 0, 2, 0));
    CHECK(!valid_pending_transaction_domain(kTransferExtensionToExtension, 2, 0, kBagCount, 1));
    CHECK(!valid_transaction_domain(kTransferExtensionToExtension, 2, 0, 3, 4));

    Item src{};
    src.category = 401;
    src.count = 5;
    make_s2_payload(&src, src.count);
    Item committed = src;
    committed.count = 9;
    CHECK(patch_payload_count(&committed, 9, host_category_uses_stack_count));

    // orig→ext pending：payload=目标提交态，source_payload=源载荷；journal 一一映射。
    TransactionContext o2e{};
    o2e.direction = kTransferOriginalToExtension;
    o2e.src_bag = 0;
    o2e.src_slot = 2;
    o2e.dst_bag = 1;
    o2e.dst_slot = 3;
    std::strncpy(o2e.transaction_id, "p4-test-1", kMaxTransactionIdChars);
    o2e.source = src;
    o2e.committed = committed;
    o2e.previous_dst.category = 7;
    o2e.previous_dst.count = 1;
    txn_build_pending(&o2e);
    CHECK(o2e.pending.valid);
    CHECK_EQ((int)o2e.pending.direction, (int)kTransferOriginalToExtension);
    CHECK_EQ((int)o2e.pending.src_bag, 0);
    CHECK_EQ((int)o2e.pending.src_slot, 2);
    CHECK_EQ((int)o2e.pending.dst_bag, 1);
    CHECK_EQ((int)o2e.pending.dst_slot, 3);
    CHECK(std::strcmp(o2e.pending.transaction_id, "p4-test-1") == 0);
    CHECK_EQ(o2e.pending.payload_size, committed.payload_size);
    CHECK(std::memcmp(o2e.pending.payload.data(), committed.payload.data(),
                      committed.payload_size) == 0);
    CHECK_EQ(o2e.pending.source_payload_size, src.payload_size);
    CHECK(std::memcmp(o2e.pending.source_payload.data(), src.payload.data(),
                      src.payload_size) == 0);

    JournalRecord journal = journal_from_pending(o2e.pending, 7);
    CHECK(journal.valid);
    CHECK_EQ((int)journal.stage, (int)kJournalStagePrepared);
    CHECK_EQ(journal.generation, (uint64_t)7);
    CHECK(std::strcmp(journal.transaction_id, "p4-test-1") == 0);
    CHECK(std::memcmp(journal.payload.data(), o2e.pending.payload.data(),
                      kSerializedItemBuffer) == 0);
    CHECK(std::memcmp(journal.source_payload.data(), o2e.pending.source_payload.data(),
                      kSerializedItemBuffer) == 0);
    CHECK(valid_journal_record(journal));

    // 空事务 id 拒绝构造 pending。
    TransactionContext no_id = o2e;
    no_id.transaction_id[0] = '\0';
    no_id.pending = {};
    txn_build_pending(&no_id);
    CHECK(!no_id.pending.valid);

    // ext→orig pending：payload=源载荷，无 source_payload（src=内部扩展袋，dst=原版袋）。
    TransactionContext e2o{};
    e2o.direction = kTransferExtensionToOriginal;
    e2o.src_bag = 1;
    e2o.src_slot = 1;
    e2o.dst_bag = 2;
    std::strncpy(e2o.transaction_id, "p4-test-2", kMaxTransactionIdChars);
    e2o.source = src;
    e2o.committed = src;
    txn_build_pending(&e2o);
    CHECK(e2o.pending.valid);
    CHECK_EQ(e2o.pending.payload_size, src.payload_size);
    CHECK_EQ((int)e2o.pending.source_payload_size, 0);

    // ext→ext pending：payload=committed。
    TransactionContext e2e{};
    e2e.direction = kTransferExtensionToExtension;
    e2e.src_bag = 2;
    e2e.src_slot = 0;
    e2e.dst_bag = 3;
    e2e.dst_slot = 4;
    std::strncpy(e2e.transaction_id, "p4-test-3", kMaxTransactionIdChars);
    e2e.source = src;
    e2e.committed = committed;
    txn_build_pending(&e2e);
    CHECK(valid_pending_transfer_domain(e2e.pending));
    CHECK_EQ(e2e.pending.payload_size, committed.payload_size);
    CHECK_EQ((int)e2e.pending.source_payload_size, 0);

    // ---- 状态边界失败注入 ----
    State st{};
    st.types = {4, 4, 4, 0, 0};
    normalize(&st);

    // orig→ext：logical 应用 → 目标=committed；kOriginalUpdated 失败 → 目标还原、pending 清。
    st.items[1][3] = o2e.previous_dst;
    st.items[0][2] = {};
    st.pending = {};
    txn_apply_logical(&st, o2e);
    CHECK(item_matches_payload(st.items[1][3], o2e.pending));
    st.pending = o2e.pending;
    txn_rollback_logical(&st, o2e, TxnStage::kOriginalUpdated);
    CHECK(p44_item_identity(st.items[1][3], o2e.previous_dst));
    CHECK(!st.pending.valid);

    // orig→ext：kPendingRecorded 失败 → 物品未提交还原（仍为 previous）、pending 清。
    st.items[1][3] = o2e.committed;
    st.pending = o2e.pending;
    txn_rollback_logical(&st, o2e, TxnStage::kPendingRecorded);
    CHECK(p44_item_identity(st.items[1][3], o2e.committed));
    CHECK(!st.pending.valid);

    // committed 后不做猜测性逆转。
    st.pending = o2e.pending;
    txn_rollback_logical(&st, o2e, TxnStage::kCommitted);
    CHECK(st.pending.valid);
    st.pending = {};

    // ext→ext：双槽应用；kLogicalUpdated 失败 → source/previous_dst 双还原、pending 清。
    e2e.previous_dst = src;
    st.items[2][0] = e2e.source;
    st.items[3][4] = src;
    st.pending = e2e.pending;
    txn_apply_logical(&st, e2e);
    CHECK(st.items[2][0].category == 0 && st.items[2][0].count == 0);
    CHECK(item_matches_payload(st.items[3][4], e2e.pending));
    txn_rollback_logical(&st, e2e, TxnStage::kLogicalUpdated);
    CHECK(p44_item_identity(st.items[2][0], e2e.source));
    CHECK(p44_item_identity(st.items[3][4], e2e.previous_dst));
    CHECK(!st.pending.valid);

    // 双非空 ext→ext swap：两槽 descriptor 互换，失败回滚恢复原值。
    TransactionContext swap = e2e;
    swap.src_bag = 2;
    swap.src_slot = 0;
    swap.dst_bag = 3;
    swap.dst_slot = 4;
    swap.swapped = true;
    swap.source = src;
    swap.previous_dst = committed;
    st.items[2][0] = swap.source;
    st.items[3][4] = swap.previous_dst;
    txn_apply_logical(&st, swap);
    CHECK(p44_item_identity(st.items[2][0], swap.previous_dst));
    CHECK(p44_item_identity(st.items[3][4], swap.source));
    txn_rollback_logical(&st, swap, TxnStage::kLogicalUpdated);
    CHECK(p44_item_identity(st.items[2][0], swap.source));
    CHECK(p44_item_identity(st.items[3][4], swap.previous_dst));
    CHECK(!st.pending.valid);

    // ext→orig：logical 阶段不改逻辑数组（清源发生在原版接管成功后）。
    st.items[1][1] = e2o.source;
    st.pending = e2o.pending;
    txn_apply_logical(&st, e2o);
    CHECK(p44_item_identity(st.items[1][1], e2o.source));
    txn_rollback_logical(&st, e2o, TxnStage::kOriginalUpdated);
    CHECK(p44_item_identity(st.items[1][1], e2o.source));
    CHECK(!st.pending.valid);

    // recovery_action：ext→ext 目标匹配 committed → kComplete；不匹配 → kRollback。
    State rs{};
    rs.pending = e2e.pending;
    rs.items[3][4] = e2e.committed;
    CHECK(recovery_action(rs, rs.pending) == RecoveryAction::kComplete);
    rs.items[3][4] = e2e.previous_dst;
    CHECK(recovery_action(rs, rs.pending) == RecoveryAction::kRollback);

    // pending JSON 往返：transactionId 与 ext→ext 方向保留。
    State ps{};
    ps.pending = e2e.pending;
    const std::string json = state_json(ps);
    PendingTransfer back{};
    CHECK(parse_pending_transfer_result(json.c_str(), &back) ==
          PendingTransferParseResult::kOk);
    CHECK(back.valid);
    CHECK(std::strcmp(back.transaction_id, "p4-test-3") == 0);
    CHECK_EQ((int)back.direction, (int)kTransferExtensionToExtension);

    // 无 id 的历史 pending JSON（向后兼容）解析为空 id。
    std::string legacy_json = json;
    const std::string id_field = ",\"transactionId\":\"p4-test-3\"";
    const size_t id_pos = legacy_json.find(id_field);
    CHECK(id_pos != std::string::npos);
    legacy_json.erase(id_pos, id_field.size());
    PendingTransfer legacy_back{};
    CHECK(parse_pending_transfer_result(legacy_json.c_str(), &legacy_back) ==
          PendingTransferParseResult::kOk);
    CHECK_EQ(legacy_back.transaction_id[0], '\0');
}

static void test_p52_drag_session() {
    using namespace virtual_bag;

    ExtensionDragSession session{};
    CHECK_EQ(session_begin(&session, 17, 42, 2, 3), DragTransition::kAdvanced);
    CHECK_EQ(session.phase, DragPhase::kPressed);
    CHECK_EQ(session_on_native_moving(&session, 42), DragTransition::kAdvanced);
    CHECK_EQ(session.phase, DragPhase::kNativeMoving);
    CHECK_EQ(session_on_native_moving(&session, 42), DragTransition::kIdempotentNoop);
    CHECK_EQ(session_resolve_target(&session, 42, DragTargetKind::kExtensionSlot),
             DragTransition::kAdvanced);
    CHECK_EQ(session.phase, DragPhase::kTargetResolved);
    CHECK_EQ(session_begin_transaction(&session, 42), DragTransition::kAdvanced);
    CHECK(session_may_create_transaction(session));
    CHECK(session_claim_transaction(&session));
    CHECK(!session_may_create_transaction(session));
    CHECK(!session_claim_transaction(&session));
    CHECK_EQ(session_on_release(&session, 42), DragTransition::kAdvanced);
    CHECK_EQ(session.release_sequence, 1u);
    CHECK_EQ(session_finish_transaction(&session, true), DragTransition::kAdvanced);
    CHECK_EQ(session.phase, DragPhase::kCommitted);
    CHECK_EQ(session_on_release(&session, 42), DragTransition::kIdempotentNoop);
    CHECK_EQ(session.release_sequence, 1u);

    ExtensionDragSession tab_session{};
    CHECK_EQ(session_begin(&tab_session, 22, 9, 0, 1), DragTransition::kAdvanced);
    CHECK_EQ(session_on_native_moving(&tab_session, 9), DragTransition::kAdvanced);
    CHECK_EQ(session_resolve_target(&tab_session, 9, DragTargetKind::kExtensionTab),
             DragTransition::kAdvanced);
    CHECK_EQ(session_begin_transaction(&tab_session, 9), DragTransition::kAdvanced);
    CHECK(session_claim_transaction(&tab_session));
    CHECK_EQ(session_finish_transaction(&tab_session, true), DragTransition::kAdvanced);
    CHECK_EQ(tab_session.phase, DragPhase::kCommitted);

    tab_session.target_kind = DragTargetKind::kOriginalBag;
    tab_session.target_bag = 2;
    tab_session.target_slot = -1;
    CHECK_EQ(tab_session.target_kind, DragTargetKind::kOriginalBag);
    CHECK_EQ(tab_session.target_bag, 2);
    CHECK_EQ(tab_session.target_slot, -1);

    ExtensionDragSession tab_illegal{};
    CHECK_EQ(session_begin(&tab_illegal, 23, 9, 0, 1), DragTransition::kAdvanced);
    CHECK_EQ(session_resolve_target(&tab_illegal, 9, DragTargetKind::kExtensionTab),
             DragTransition::kIllegal);

    // 非法跳转不能绕过原生 moving、目标解析或事务阶段。
    ExtensionDragSession illegal{};
    CHECK_EQ(session_begin_transaction(&illegal, 0), DragTransition::kIllegal);
    CHECK_EQ(session_on_release(&illegal, 0), DragTransition::kIllegal);
    CHECK_EQ(session_begin(&illegal, 18, 42, 1, 1), DragTransition::kAdvanced);
    CHECK_EQ(session_begin(&illegal, 19, 42, 1, 1), DragTransition::kIllegal);
    CHECK_EQ(illegal.target_kind, DragTargetKind::kInvalid);
    CHECK_EQ(illegal.target_bag, -1);
    CHECK_EQ(session_resolve_target(&illegal, 42, DragTargetKind::kOriginalBag),
             DragTransition::kIllegal);

    // stale generation 和已取消 session 都只做幂等 no-op，不创建事务。
    CHECK_EQ(session_on_native_moving(&illegal, 41), DragTransition::kIdempotentNoop);
    CHECK_EQ(session_on_cancel(&illegal, 42), DragTransition::kAdvanced);
    CHECK_EQ(session_on_cancel(&illegal, 42), DragTransition::kIdempotentNoop);
    CHECK_EQ(session_on_release(&illegal, 42), DragTransition::kIdempotentNoop);
    CHECK(!session_may_create_transaction(illegal));
    CHECK_EQ(session_begin_transaction(&illegal, 42), DragTransition::kIllegal);

    ExtensionDragSession cancel_from_target{};
    CHECK_EQ(session_begin(&cancel_from_target, 20, 7, 0, 0), DragTransition::kAdvanced);
    CHECK_EQ(session_on_native_moving(&cancel_from_target, 7), DragTransition::kAdvanced);
    CHECK_EQ(session_resolve_target(&cancel_from_target, 7, DragTargetKind::kOriginalBag),
             DragTransition::kAdvanced);
    CHECK_EQ(session_on_cancel(&cancel_from_target, 7), DragTransition::kAdvanced);
    CHECK_EQ(cancel_from_target.phase, DragPhase::kCancelled);

    // 无效目标进入 terminal rejected；任务袋 sentinel 与扩展/原版编号空间隔离。
    ExtensionDragSession rejected{};
    CHECK_EQ(session_begin(&rejected, 21, 8, 0, 0), DragTransition::kAdvanced);
    CHECK_EQ(session_on_native_moving(&rejected, 8), DragTransition::kAdvanced);
    CHECK_EQ(session_resolve_target(&rejected, 8, DragTargetKind::kTaskBagRejected),
             DragTransition::kAdvanced);
    CHECK_EQ(rejected.phase, DragPhase::kRejected);
    CHECK_EQ(session_on_cancel(&rejected, 8), DragTransition::kIllegal);
    CHECK(!session_may_create_transaction(rejected));

    CHECK_EQ(classify_drag_target(0, 0), DragTargetKind::kOriginalBag);
    CHECK_EQ(classify_drag_target(kOriginalTaskBag, 0), DragTargetKind::kTaskBagRejected);
    CHECK_EQ(classify_drag_target(kExtensionLogicalBagFirst, 4), DragTargetKind::kExtensionSlot);
    CHECK_EQ(classify_drag_target(kExtensionLogicalBagLast, 15), DragTargetKind::kExtensionSlot);
    CHECK_EQ(classify_drag_target(kExtensionLogicalBagFirst, 0, true),
             DragTargetKind::kExtensionTab);
    CHECK_EQ(classify_drag_target(kBagCount, 0), DragTargetKind::kTaskBagRejected);
    CHECK_EQ(classify_drag_target(0, kSlotCount), DragTargetKind::kInvalid);
    CHECK_EQ(classify_drag_target(0, 0, false, true), DragTargetKind::kCancel);
}

static void test_p45_isolation() {
    using virtual_bag::IsolationRecord;
    namespace ir = virtual_bag::isolation_reason;

    // reason 分类互异（host/native 单一来源）。
    const char* reasons[] = {
        ir::kPayloadInvalid,
        ir::kInvalidTransactionDomain,
        ir::kInvalidPayload,
        ir::kLoadFailed,
        ir::kInsertFailed,
        ir::kSlotNotFound,
        ir::kJournalInvalid,
    };
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        CHECK(reasons[i][0] != '\0');
        for (size_t j = i + 1; j < sizeof(reasons) / sizeof(reasons[0]); ++j) {
            CHECK(std::strcmp(reasons[i], reasons[j]) != 0);
        }
    }

    // payload_invalid：normalize 保留原始字节为隔离记录后清槽，detail 区分完整性诊断。
    virtual_bag::State state{};
    state.isolation_now_ms = 1234;
    state.items[2][3].category = 401;
    state.items[2][3].count = 1;
    state.items[2][3].payload_size = 5;
    state.items[2][3].payload[0] = 4;
    state.items[2][3].payload[1] = 9;
    virtual_bag::normalize(&state);
    CHECK_EQ(static_cast<int>(state.items[2][3].payload_size), 0);
    CHECK_EQ(state.items[2][3].category, 0);
    CHECK_EQ(state.items[2][3].count, 0);
    CHECK_EQ(state.isolation_count, 1);
    CHECK_EQ(state.isolation_sequence, 1);
    const IsolationRecord& normalized = state.isolations[0];
    CHECK(normalized.valid);
    CHECK_EQ(normalized.sequence, 0u);
    CHECK_EQ(normalized.observed_at_ms, 1234u);
    CHECK(std::strcmp(normalized.reason, ir::kPayloadInvalid) == 0);
    CHECK_EQ(normalized.src_bag, 2);
    CHECK_EQ(normalized.src_slot, 3);
    CHECK_EQ(static_cast<int>(normalized.payload_size), 5);
    CHECK_EQ(normalized.payload[1], 9);
    CHECK(std::strcmp(normalized.detail, "payload_length_out_of_range") == 0);

    // category_invalid detail 分支：payload 校验通过但类别非法。
    virtual_bag::State bad_category{};
    bad_category.items[0][0].category = 0;
    bad_category.items[0][0].count = 1;
    bad_category.items[0][0].payload_size = 20;
    bad_category.items[0][0].payload[0] = 19;
    virtual_bag::normalize(&bad_category);
    CHECK_EQ(bad_category.isolation_count, 1);
    CHECK(std::strcmp(bad_category.isolations[0].detail, "category_invalid") == 0);

    // D3：payload_size 超出固定数组容量时隔离记录钳制，序列化不越界读。
    virtual_bag::State oversized{};
    oversized.items[1][1].category = 401;
    oversized.items[1][1].count = 1;
    oversized.items[1][1].payload_size = 300;
    oversized.items[1][1].payload[0] = 7;
    virtual_bag::normalize(&oversized);
    CHECK_EQ(oversized.isolation_count, 1);
    CHECK_EQ(static_cast<int>(oversized.isolations[0].payload_size), 256);
    const std::string oversized_json = virtual_bag::state_json(oversized, true);
    CHECK(oversized_json.find("\"isolations\"") != std::string::npos);
    CHECK(oversized_json.find("payload_invalid") != std::string::npos);

    // 环形覆盖：写满 8 条后覆盖最旧，sequence 全局单调。
    virtual_bag::State ring{};
    for (int round = 0; round < 10; ++round) {
        IsolationRecord pushed{};
        pushed.valid = true;
        push_isolation(&ring, pushed);
    }
    CHECK_EQ(static_cast<int>(ring.isolation_count), 8);
    CHECK_EQ(ring.isolation_sequence, 10u);
    const size_t oldest =
        (ring.isolation_next + virtual_bag::kMaxIsolationRecords - ring.isolation_count) %
        virtual_bag::kMaxIsolationRecords;
    CHECK_EQ(ring.isolations[oldest].sequence, 2u);
    const size_t newest = (ring.isolation_next + virtual_bag::kMaxIsolationRecords - 1) %
                          virtual_bag::kMaxIsolationRecords;
    CHECK_EQ(ring.isolations[newest].sequence, 9u);

    // journal kDiscard：非法 journal（域越界）被拒并隔离，原始值不正则化。
    virtual_bag::JournalRecord bad{};
    bad.valid = true;
    bad.stage = virtual_bag::kJournalStageOriginalSaved;
    bad.direction = 200;
    bad.src_bag = 200;
    bad.src_slot = 250;
    bad.dst_bag = 255;
    bad.dst_slot = 254;
    bad.generation = 42;
    std::strncpy(bad.transaction_id, "crash-txn", sizeof(bad.transaction_id) - 1);
    bad.payload_size = 32;
    for (int index = 0; index < 32; ++index) bad.payload[static_cast<size_t>(index)] = static_cast<uint8_t>(index);
    bad.source_payload_size = 20;
    bad.source_payload[0] = 19;
    virtual_bag::WorldProbe probe{};
    CHECK(virtual_bag::journal_recovery_action(state, bad, probe) ==
          virtual_bag::JournalRecovery::kDiscard);
    const IsolationRecord journal_record =
        virtual_bag::isolation_from_journal_record(bad, 777);
    CHECK(journal_record.valid);
    CHECK(std::strcmp(journal_record.reason, ir::kJournalInvalid) == 0);
    CHECK_EQ(journal_record.observed_at_ms, 777u);
    CHECK_EQ(journal_record.generation, 42u);
    CHECK_EQ(journal_record.direction, 200);
    CHECK_EQ(journal_record.phase, static_cast<int>(virtual_bag::kJournalStageOriginalSaved));
    CHECK_EQ(journal_record.src_bag, 200);
    CHECK_EQ(journal_record.src_slot, 250);
    CHECK_EQ(journal_record.dst_bag, 255);
    CHECK_EQ(journal_record.dst_slot, 254);
    CHECK(std::strcmp(journal_record.transaction_id, "crash-txn") == 0);
    CHECK_EQ(static_cast<int>(journal_record.payload_size), 32);
    CHECK_EQ(journal_record.payload[31], 31);
    CHECK_EQ(static_cast<int>(journal_record.source_payload_size), 20);
    CHECK(std::strcmp(journal_record.detail, "kDiscard") == 0);

    // transaction_id 超长安全截断。
    virtual_bag::JournalRecord long_id{};
    std::memset(long_id.transaction_id, 'x', sizeof(long_id.transaction_id) - 1);
    long_id.transaction_id[sizeof(long_id.transaction_id) - 1] = '\0';
    const IsolationRecord truncated =
        virtual_bag::isolation_from_journal_record(long_id, 0);
    CHECK_EQ(std::strlen(truncated.transaction_id),
             static_cast<size_t>(virtual_bag::kMaxTransactionIdChars));

    // state_json：include_isolations=true 输出只读诊断；默认/写路径不含。
    virtual_bag::State with_record{};
    with_record.isolation_now_ms = 55;
    with_record.items[1][2].category = 401;
    with_record.items[1][2].count = 1;
    with_record.items[1][2].payload_size = 5;
    with_record.items[1][2].payload[0] = 4;
    virtual_bag::normalize(&with_record);
    IsolationRecord enriched{};
    enriched.valid = true;
    std::strncpy(enriched.transaction_id, "txn-9", sizeof(enriched.transaction_id) - 1);
    enriched.direction = 0;
    enriched.phase = 1;
    enriched.src_bag = 1;
    enriched.src_slot = 2;
    enriched.dst_bag = 3;
    enriched.dst_slot = 4;
    enriched.payload_size = 3;
    enriched.payload[0] = 'a';
    enriched.payload[1] = 'b';
    enriched.payload[2] = 'c';
    push_isolation(&with_record, enriched);
    const std::string with_isolations = state_json(with_record, true);
    CHECK(with_isolations.find("\"isolations\":[") != std::string::npos);
    CHECK(with_isolations.find("\"recordId\":1") != std::string::npos);
    CHECK(with_isolations.find("\"reason\":\"payload_invalid\"") != std::string::npos);
    CHECK(with_isolations.find("\"transactionId\":\"txn-9\"") != std::string::npos);
    CHECK(with_isolations.find("\"srcBag\":1") != std::string::npos);
    CHECK(with_isolations.find("\"payload\":\"YWJj\"") != std::string::npos);
    CHECK(with_isolations.find("\"payloadLength\":3") != std::string::npos);
    CHECK(with_isolations.find("\"detail\":\"payload_length_out_of_range\"") != std::string::npos);
    const std::string without_isolations = state_json(with_record);
    CHECK(without_isolations.find("\"isolations\"") == std::string::npos);
    CHECK(state_json(with_record, false).find("\"isolations\"") == std::string::npos);

    // 隔离记录不进 JSON 往返（sidecar section 无隔离字段，P7 决定落盘时机）。
    const std::string roundtrip_json = state_json(with_record);
    virtual_bag::State reparsed{};
    CHECK(virtual_bag::parse_state_json(roundtrip_json.c_str(), &reparsed));
    CHECK_EQ(static_cast<int>(reparsed.isolation_count), 0);
}

int main() {
    test_p7_stage4_find_item_poc();
    test_world_teleport_target_map_id();
    test_json_escape();
    test_base64_decode();
    test_parse_int_field();
    test_tiles_parse();
    test_ownership_ledger();
    test_ownership_ledger_p43();
    test_p44_transaction_stages();
    test_p52_drag_session();
    test_p45_isolation();
    test_unequip_bag();
    test_equip_bag();
    test_nav_bfs();
    test_nav_bfs_multi();
    test_stack_codec();
    test_stack_codec_s2();
    test_stack_codec_mode_invariance();
    test_stack_codec_effective_mode();
    test_sell_price_bounds();
    test_virtual_bag_state();
    test_extension_bag_exit_rendering_state();
    test_prepare_journal();
    test_virtual_bag_base64();
    test_virtual_bag_payload_helpers();
    test_virtual_bag_payload_bridge();
    test_virtual_bag_merge_count();
    test_virtual_bag_mode_aware_ops();
    test_virtual_bag_mergeable_items();
    test_sell_divide_s2_read_window();
    test_adopt_merge_plan();
    test_place_plan();
    test_ext2orig_host_guards();
    test_virtual_bag_json_roundtrip();
    test_virtual_bag_json_count_clamp();
    test_virtual_bag_cross_config_roundtrip();
    test_virtual_bag_legacy_json();
    test_virtual_bag_normalize_payload();
    test_virtual_bag_recovery();
    test_virtual_bag_transaction_domain();
    test_save_preflight_classify();
    test_save_preflight_stage();
    test_save_preflight_json();
    test_save_backup_digests();
    test_save_backup_base64();
    test_save_backup_bundle_roundtrip();
    test_save_backup_meta_entry();
    test_save_backup_entry_map_name();
    test_save_backup_checksum_name();
    test_save_backup_module_container();
    test_save_backup_warehouse();

    std::printf("host_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
