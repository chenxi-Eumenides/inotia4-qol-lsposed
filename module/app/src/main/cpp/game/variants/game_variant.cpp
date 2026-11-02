// game_variant.cpp —— libgame.so 校验值 → 游戏变体/能力 的运行时查表实现。
//
// 依赖方向：game 层，仅依赖 STL 与 game/access/qol_log_seam.h（过渡日志缝）。
// 流程：/proc/self/maps 定位 libgame.so → 算 md5 → 查离线表 → 未命中扫描库内已知签名。
#include "game/variants/game_variant.h"

#include "game/access/qol_log_seam.h"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace qol {
namespace {

constexpr const char* kLibgameName = "libgame.so";
constexpr const char* kWh4Marker = "WH4JRN01";
constexpr size_t kWh4MarkerLen = 8;
constexpr char kC18BagMarker[] = "C18BAG01";
constexpr size_t kC18BagMarkerLen = sizeof(kC18BagMarker) - 1;
constexpr char kC18BagDataMarker[] = "C18Lbag.dat";
constexpr size_t kC18BagDataMarkerLen = sizeof(kC18BagDataMarker) - 1;

bool contains_marker(const uint8_t* data, size_t size, const char* marker, size_t marker_size) {
    if (data == nullptr || marker == nullptr || marker_size == 0 || size < marker_size) return false;
    for (size_t offset = 0; offset <= size - marker_size; ++offset) {
        if (std::memcmp(data + offset, marker, marker_size) == 0) return true;
    }
    return false;
}

bool has_c18_native_bag_layout_candidate_signature(const uint8_t* data, size_t size) {
    return contains_marker(data, size, kC18BagMarker, kC18BagMarkerLen) &&
           contains_marker(data, size, kC18BagDataMarker, kC18BagDataMarkerLen);
}

// ---------------------------------------------------------------------------
// 紧凑 MD5（RFC 1321），无第三方依赖。
// ---------------------------------------------------------------------------
struct Md5Context {
    uint32_t state[4];
    uint64_t total_bytes;
    uint8_t buffer[64];
    size_t buffer_len;
};

constexpr uint32_t kMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

constexpr uint8_t kMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

inline uint32_t rotl32(uint32_t v, uint8_t n) { return (v << n) | (v >> (32 - n)); }

void md5_init(Md5Context& ctx) {
    ctx.state[0] = 0x67452301u;
    ctx.state[1] = 0xefcdab89u;
    ctx.state[2] = 0x98badcfeu;
    ctx.state[3] = 0x10325476u;
    ctx.total_bytes = 0;
    ctx.buffer_len = 0;
}

void md5_transform(Md5Context& ctx, const uint8_t block[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }
    uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f += a + kMd5K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += rotl32(f, kMd5Shift[i]);
    }
    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
}

void md5_update(Md5Context& ctx, const uint8_t* data, size_t len) {
    ctx.total_bytes += len;
    size_t offset = 0;
    if (ctx.buffer_len > 0) {
        const size_t need = 64 - ctx.buffer_len;
        const size_t take = len < need ? len : need;
        std::memcpy(ctx.buffer + ctx.buffer_len, data, take);
        ctx.buffer_len += take;
        offset += take;
        if (ctx.buffer_len == 64) {
            md5_transform(ctx, ctx.buffer);
            ctx.buffer_len = 0;
        }
    }
    for (; offset + 64 <= len; offset += 64) {
        md5_transform(ctx, data + offset);
    }
    if (offset < len) {
        std::memcpy(ctx.buffer, data + offset, len - offset);
        ctx.buffer_len = len - offset;
    }
}

void md5_final(Md5Context& ctx, uint8_t out[16]) {
    const uint64_t bit_len = ctx.total_bytes * 8;
    static const uint8_t kPad[64] = {0x80};
    const size_t pad_len = (ctx.buffer_len < 56) ? (56 - ctx.buffer_len)
                                                 : (120 - ctx.buffer_len);
    md5_update(ctx, kPad, pad_len);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
        len_bytes[i] = static_cast<uint8_t>((bit_len >> (8 * i)) & 0xffu);
    }
    md5_update(ctx, len_bytes, sizeof(len_bytes));
    for (int i = 0; i < 4; ++i) {
        out[i * 4] = static_cast<uint8_t>(ctx.state[i] & 0xffu);
        out[i * 4 + 1] = static_cast<uint8_t>((ctx.state[i] >> 8) & 0xffu);
        out[i * 4 + 2] = static_cast<uint8_t>((ctx.state[i] >> 16) & 0xffu);
        out[i * 4 + 3] = static_cast<uint8_t>((ctx.state[i] >> 24) & 0xffu);
    }
}

void md5_hex(const uint8_t md5[16], char out[33]) {
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = kHex[(md5[i] >> 4) & 0x0fu];
        out[i * 2 + 1] = kHex[md5[i] & 0x0fu];
    }
    out[32] = '\0';
}

// ---------------------------------------------------------------------------
// 运行时定位与探测
// ---------------------------------------------------------------------------
struct MappingLine {
    uintptr_t lo;
    uintptr_t hi;
    char perms[8];
    char path[512];
};

// 解析 /proc/self/maps 的一行；成功返回 true。path 取最后一个字段（可含空格由 %[^\n] 兜住）。
bool parse_mapping_line(const char* line, MappingLine& out) {
    unsigned long long lo = 0, hi = 0;
    char perms[8] = {0};
    char rest[576] = {0};
    // 格式：start-end perms offset dev inode pathname
    if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %575[^\n]", &lo, &hi, perms, rest) < 4) {
        return false;
    }
    out.lo = static_cast<uintptr_t>(lo);
    out.hi = static_cast<uintptr_t>(hi);
    std::memcpy(out.perms, perms, sizeof(perms));
    // 去掉前导/尾随空白
    char* p = rest;
    while (*p == ' ' || *p == '\t') ++p;
    size_t n = std::strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\n' || p[n - 1] == '\r')) {
        p[--n] = '\0';
    }
    std::snprintf(out.path, sizeof(out.path), "%s", p);
    return true;
}

bool path_is_libgame(const char* path) {
    if (path == nullptr) return false;
    const size_t name_len = std::strlen(kLibgameName);
    const char* match = path;
    while ((match = std::strstr(match, kLibgameName)) != nullptr) {
        const bool starts_component = match == path || match[-1] == '/' || match[-1] == '!';
        const char suffix = match[name_len];
        const bool ends_component = suffix == '\0' || suffix == ' ' || suffix == '!';
        if (starts_component && ends_component) return true;
        ++match;
    }
    return false;
}

// 取首个 libgame.so 映射的 pathname；找不到返回 false。
bool find_libgame_path(char* out, size_t cap) {
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) return false;
    char line[1024];
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        MappingLine mapping{};
        if (!parse_mapping_line(line, mapping)) continue;
        if (!path_is_libgame(mapping.path)) continue;
        std::snprintf(out, cap, "%s", mapping.path);
        found = true;
        break;
    }
    std::fclose(f);
    return found;
}

struct RuntimeMarkers {
    bool has_wh4_marker = false;
    bool has_wh4_executable_marker = false;
    bool has_c18_bag_marker = false;
    bool has_c18_bag_data_marker = false;
};

struct MemoryMappingView {
    const uint8_t* data;
    size_t size;
    const char* permissions;
    const char* path;
};

void collect_mapping_markers(const MemoryMappingView& mapping, RuntimeMarkers& markers) {
    if (mapping.data == nullptr || mapping.permissions == nullptr || mapping.path == nullptr ||
        std::strchr(mapping.permissions, 'r') == nullptr || !path_is_libgame(mapping.path)) {
        return;
    }
    const bool mapping_has_wh4 = contains_marker(mapping.data, mapping.size, kWh4Marker, kWh4MarkerLen);
    if (mapping_has_wh4) {
        markers.has_wh4_marker = true;
        if (std::strchr(mapping.permissions, 'x') != nullptr) {
            markers.has_wh4_executable_marker = true;
        }
    }
    if (!markers.has_c18_bag_marker) {
        markers.has_c18_bag_marker =
            contains_marker(mapping.data, mapping.size, kC18BagMarker, kC18BagMarkerLen);
    }
    if (!markers.has_c18_bag_data_marker) {
        markers.has_c18_bag_data_marker =
            contains_marker(mapping.data, mapping.size, kC18BagDataMarker, kC18BagDataMarkerLen);
    }
}

// 检查未知 MD5 对应的 libgame 映射；绝不扫描其他进程映射。
RuntimeMarkers scan_libgame_markers(void*) {
    RuntimeMarkers markers{};
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) return markers;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        MappingLine mapping{};
        if (!parse_mapping_line(line, mapping) || !path_is_libgame(mapping.path) ||
            std::strchr(mapping.perms, 'r') == nullptr || mapping.hi <= mapping.lo) {
            continue;
        }
        const size_t mapping_size = static_cast<size_t>(mapping.hi - mapping.lo);
        const MemoryMappingView view{reinterpret_cast<const uint8_t*>(mapping.lo), mapping_size,
                                     mapping.perms, mapping.path};
        collect_mapping_markers(view, markers);
        if (markers.has_wh4_executable_marker && markers.has_c18_bag_marker &&
            markers.has_c18_bag_data_marker) {
            break;
        }
    }
    std::fclose(f);
    return markers;
}

// 读取文件全部内容并算 md5；成功返回 true。
bool md5_of_file(const char* path, uint8_t out[16]) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    Md5Context ctx;
    md5_init(ctx);
    uint8_t buffer[64 * 1024];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        md5_update(ctx, buffer, n);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) return false;
    md5_final(ctx, out);
    return true;
}

// ---------------------------------------------------------------------------
// 离线表（由 scripts/maintenance/build_game_variant_table.py 生成）
// ---------------------------------------------------------------------------
#include "game/variants/game_variant_table.inc"

// ---------------------------------------------------------------------------
// 缓存状态
// ---------------------------------------------------------------------------
std::mutex g_variant_mutex;
bool g_variant_initialized = false;
GameVariant g_variant;

const GameVariant kUnknownVariant{};

}  // namespace

namespace game_variant_detail {

bool md5_hex_equals(const uint8_t md5[16], const char* hex32) {
    if (hex32 == nullptr || std::strlen(hex32) != 32) return false;
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        const char hi = hex32[i * 2];
        const char lo = hex32[i * 2 + 1];
        const char* hi_pos = std::strchr(kHex, hi >= 'A' && hi <= 'F' ? static_cast<char>(hi + 32) : hi);
        const char* lo_pos = std::strchr(kHex, lo >= 'A' && lo <= 'F' ? static_cast<char>(lo + 32) : lo);
        if (hi_pos == nullptr || lo_pos == nullptr) return false;
        const uint8_t expected = static_cast<uint8_t>(((hi_pos - kHex) << 4) | (lo_pos - kHex));
        if (md5[i] != expected) return false;
    }
    return true;
}

const GameVariant* lookup(const uint8_t md5[16]) {
    for (size_t i = 0; i < kGameVariantTableSize; ++i) {
        if (std::memcmp(kGameVariantTable[i].md5, md5, 16) == 0) {
            return &kGameVariantTable[i];
        }
    }
    return nullptr;
}

GameSeries classify_fallback(bool has_wh4_marker) {
    return has_wh4_marker ? GameSeries::kMonster : GameSeries::kUnknown;
}

static GameVariant fallback_variant(const uint8_t md5[16], bool has_wh4_marker,
                                    bool has_c18_signature) {
    GameVariant result{};
    result.series = classify_fallback(has_wh4_marker);
    if (!has_wh4_marker && has_c18_signature) {
        result.capabilities |= kCapC18NativeBagLayoutCandidate;
    }
    result.known = false;
    std::memcpy(result.md5, md5, sizeof(result.md5));
    return result;
}

static GameVariant fallback_variant(const uint8_t md5[16], const RuntimeMarkers& markers) {
    const bool has_c18_signature = markers.has_c18_bag_marker && markers.has_c18_bag_data_marker &&
                                   !markers.has_wh4_marker;
    return fallback_variant(md5, markers.has_wh4_executable_marker, has_c18_signature);
}

using RuntimeMarkerScanner = RuntimeMarkers (*)(void* context);

static GameVariant resolve_variant(const uint8_t md5[16], bool have_md5,
                                   RuntimeMarkerScanner scanner, void* scanner_context) {
    if (have_md5) {
        const GameVariant* hit = lookup(md5);
        if (hit != nullptr) {
            GameVariant result = *hit;
            result.known = true;
            return result;
        }
    }
    const RuntimeMarkers markers = scanner == nullptr ? RuntimeMarkers{} : scanner(scanner_context);
    return fallback_variant(md5, markers);
}

}  // namespace game_variant_detail

const char* game_series_name(GameSeries s) {
    switch (s) {
        case GameSeries::kUnknown: return "unknown";
        case GameSeries::kOriginal: return "original";
        case GameSeries::kOverhaul: return "overhaul";
        case GameSeries::kMonster: return "monster";
        case GameSeries::kC18: return "c18";
    }
    return "unknown";
}

bool c18_gem_table_supported(const GameVariant& variant) {
    if (!variant.known || variant.series != GameSeries::kC18) return false;
    const char* label = variant.version_label;
    if (label == nullptr || label[0] != 'v') return false;
    int version = 0;
    for (const char* p = label + 1; *p >= '0' && *p <= '9'; ++p) {
        version = version * 10 + (*p - '0');
        if (version > 100000) return false;
    }
    // v507 是首个带表驱动宝石数值的 C18L 版本；v0.2/v0.38 仍走 CAL 公式。
    return version >= 507;
}

bool c18_gem_table_available() {
    if (!game_variant_init()) return false;
    return c18_gem_table_supported(game_variant());
}

bool sell_multiplier(GameSeries series, uint32_t sale_path, uint32_t* num, uint32_t* den) {
    if (num == nullptr || den == nullptr) return false;
    if (sale_path == contracts::kStoreSell) {
        *num = 10;
        *den = 10;
        return true;
    }
    if (sale_path != contracts::kDetailSell) return false;

    *den = 10;
    switch (series) {
        case GameSeries::kOriginal:
        case GameSeries::kUnknown:
            *num = 0;
            return true;
        case GameSeries::kOverhaul:
        case GameSeries::kMonster:
            *num = 7;
            return true;
        case GameSeries::kC18:
            *num = 17;
            *den = 20;
            return true;
    }
    *num = 0;
    return true;
}

bool game_variant_init() {
    std::lock_guard<std::mutex> lock(g_variant_mutex);
    if (g_variant_initialized) return true;

    uint8_t md5[16] = {0};
    bool have_md5 = false;

    char path[512] = {0};
    const bool have_path = find_libgame_path(path, sizeof(path));
    if (!have_path) {
        QOL_LOG_SEAM_WARN(qol::game::kQolLogDomainPlatform, "game_variant: libgame.so mapping not found");
    } else if (std::strchr(path, '!') != nullptr) {
        QOL_LOG_SEAM_WARN(qol::game::kQolLogDomainPlatform,
                          "game_variant: libgame.so mapped from archive (path contains '!'), fallback scan");
    } else {
        have_md5 = md5_of_file(path, md5);
        if (!have_md5) {
            QOL_LOG_SEAM_WARN(qol::game::kQolLogDomainPlatform, "game_variant: cannot read %s, fallback scan", path);
        }
    }

    const GameVariant result =
        game_variant_detail::resolve_variant(md5, have_md5, scan_libgame_markers, nullptr);

    char md5_text[33];
    md5_hex(result.md5, md5_text);
    QOL_LOG_SEAM_INFO(qol::game::kQolLogDomainPlatform, "game_variant: series=%s version=%s caps=0x%x md5=%s known=%d",
                 game_series_name(result.series), result.version_label, result.capabilities,
                 md5_text, result.known ? 1 : 0);

    g_variant = result;
    g_variant_initialized = true;
    return true;
}

const GameVariant& game_variant() {
    std::lock_guard<std::mutex> lock(g_variant_mutex);
    if (!g_variant_initialized) return kUnknownVariant;
    return g_variant;
}

}  // namespace qol
