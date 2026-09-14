// host 单测：libgame.so 变体对照表运行时查表（MD5、表命中、回退分类、系列名）。
//
// 与 test_host.cpp 同法：直接 #include 被测 .cpp，以访问匿名命名空间的 MD5 内部函数。
// Android 专属头（<android/log.h>）由 stubs/ 覆盖，qol_log.cpp 单链接；不依赖设备与 I/O。
#include "core/native/game_variant.cpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace qol {
namespace {

std::string md5_hex_of(const char* text) {
    uint8_t out[16];
    Md5Context ctx;
    md5_init(ctx);
    md5_update(ctx, reinterpret_cast<const uint8_t*>(text), std::strlen(text));
    md5_final(ctx, out);
    char hex[33];
    md5_hex(out, hex);
    return std::string(hex);
}

}  // namespace
}  // namespace qol

static void test_md5_known_vectors() {
    CHECK(qol::md5_hex_of("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(qol::md5_hex_of("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(qol::md5_hex_of("The quick brown fox jumps over the lazy dog") ==
          "9e107d9d372bb6826bd81d3542a419d6");
}

static void test_md5_incremental_matches_single_shot() {
    // 多段喂入（跨 64 字节边界）结果与一次喂入一致。
    const char* text = "The quick brown fox jumps over the lazy dog";
    uint8_t out[16];
    qol::Md5Context ctx;
    qol::md5_init(ctx);
    qol::md5_update(ctx, reinterpret_cast<const uint8_t*>(text), 10);
    qol::md5_update(ctx, reinterpret_cast<const uint8_t*>(text + 10), std::strlen(text) - 10);
    qol::md5_final(ctx, out);
    char hex[33];
    qol::md5_hex(out, hex);
    CHECK(std::strcmp(hex, "9e107d9d372bb6826bd81d3542a419d6") == 0);
}

static void test_md5_hex_equals() {
    // monster v26 的实算 md5。
    const uint8_t md5[16] = {0x9b, 0xc2, 0x1b, 0x18, 0xd3, 0x43, 0x46, 0x80,
                             0x25, 0x78, 0xd0, 0x6f, 0x62, 0x89, 0xd5, 0xd1};
    CHECK(qol::game_variant_detail::md5_hex_equals(md5, "9bc21b18d34346802578d06f6289d5d1"));
    CHECK(qol::game_variant_detail::md5_hex_equals(md5, "9BC21B18D34346802578D06F6289D5D1"));
    CHECK(!qol::game_variant_detail::md5_hex_equals(md5, "9bc21b18d34346802578d06f6289d5d2"));
    CHECK(!qol::game_variant_detail::md5_hex_equals(md5, "9bc21b18"));
    CHECK(!qol::game_variant_detail::md5_hex_equals(md5, "9bc21b18d34346802578d06f6289d5d"));
    CHECK(!qol::game_variant_detail::md5_hex_equals(md5, "9bc21b18d34346802578d06f6289d5dz"));
    CHECK(!qol::game_variant_detail::md5_hex_equals(md5, nullptr));
}

static void test_lookup_real_entry() {
    const uint8_t md5[16] = {0x9b, 0xc2, 0x1b, 0x18, 0xd3, 0x43, 0x46, 0x80,
                             0x25, 0x78, 0xd0, 0x6f, 0x62, 0x89, 0xd5, 0xd1};
    const qol::GameVariant* hit = qol::game_variant_detail::lookup(md5);
    CHECK(hit != nullptr);
    if (hit != nullptr) {
        CHECK(hit->series == qol::GameSeries::kMonster);
        CHECK(std::strcmp(hit->version_label, "v26") == 0);
        CHECK((hit->capabilities & qol::kCapHiddenSegment) != 0);
        CHECK((hit->capabilities & qol::kCapWarehouse) != 0);
    }
}

static void test_lookup_miss() {
    const uint8_t absent[16] = {0};
    CHECK(qol::game_variant_detail::lookup(absent) == nullptr);
}

static void test_classify_fallback() {
    CHECK(qol::game_variant_detail::classify_fallback(true) == qol::GameSeries::kMonster);
    CHECK(qol::game_variant_detail::classify_fallback(false) == qol::GameSeries::kUnknown);
}

static void test_series_names() {
    CHECK(std::strcmp(qol::game_series_name(qol::GameSeries::kUnknown), "unknown") == 0);
    CHECK(std::strcmp(qol::game_series_name(qol::GameSeries::kOriginal), "original") == 0);
    CHECK(std::strcmp(qol::game_series_name(qol::GameSeries::kOverhaul), "overhaul") == 0);
    CHECK(std::strcmp(qol::game_series_name(qol::GameSeries::kMonster), "monster") == 0);
}

static void test_uninitialized_default() {
    const qol::GameVariant& v = qol::game_variant();
    CHECK(v.series == qol::GameSeries::kUnknown);
    CHECK(v.known == false);
    CHECK(std::strcmp(v.version_label, "") == 0);
}

int main() {
    test_md5_known_vectors();
    test_md5_incremental_matches_single_shot();
    test_md5_hex_equals();
    test_lookup_real_entry();
    test_lookup_miss();
    test_classify_fallback();
    test_series_names();
    test_uninitialized_default();
    std::printf("game_variant_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
