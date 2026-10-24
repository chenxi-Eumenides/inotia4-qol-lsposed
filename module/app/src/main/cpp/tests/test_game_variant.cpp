// host 单测：libgame.so 变体对照表运行时查表（MD5、表命中、回退分类、系列名）。
//
// 与 test_host.cpp 同法：直接 #include 被测 .cpp，以访问匿名命名空间的 MD5 内部函数。
// Android 专属头（<android/log.h>）由 stubs/ 覆盖，qol_log.cpp 单链接；不依赖设备与 I/O。
#include "contracts/game_host.h"
#include "game/variants/game_variant.cpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

static void test_lookup_c18_entry() {
    // C18 v0.2 的实算 md5（同 monster 一样按 libgame.so 校验值识别）。
    const uint8_t md5[16] = {0x47, 0x95, 0xc5, 0x1d, 0x74, 0x6b, 0xa0, 0x97,
                             0xd0, 0x28, 0x5a, 0x34, 0xde, 0x8c, 0xca, 0xd3};
    const qol::GameVariant* hit = qol::game_variant_detail::lookup(md5);
    CHECK(hit != nullptr);
    if (hit != nullptr) {
        CHECK(hit->series == qol::GameSeries::kC18);
        CHECK(std::strcmp(hit->version_label, "v0.2") == 0);
        CHECK(hit->capabilities == 0);
    }
}

static void check_c18_native_bag_layout_candidate(const uint8_t md5[16],
                                                  const char* version_label,
                                                  bool expected) {
    const qol::GameVariant* hit = qol::game_variant_detail::lookup(md5);
    CHECK(hit != nullptr);
    if (hit == nullptr) return;
    CHECK(hit->series == qol::GameSeries::kC18);
    CHECK(std::strcmp(hit->version_label, version_label) == 0);
    CHECK(((hit->capabilities & qol::kCapC18NativeBagLayoutCandidate) != 0) == expected);
}

static void test_lookup_c18_native_bag_layout_candidate() {
    CHECK(qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate ==
          qol::kCapC18NativeBagLayoutCandidate);

    // MD5 values are generated from the APKs listed in game_variant_table.inc.
    const uint8_t v507[16] = {0xfa, 0x63, 0x9c, 0x66, 0x06, 0x76, 0x31, 0x01,
                              0x6e, 0x31, 0x68, 0x2d, 0xa5, 0xeb, 0xbd, 0xf8};
    const uint8_t v508[16] = {0xc9, 0xfc, 0x6b, 0xf1, 0xf0, 0x84, 0xf6, 0xbd,
                              0x3e, 0x9c, 0xe4, 0x71, 0x67, 0xd0, 0x79, 0x4b};
    const uint8_t v512[16] = {0x48, 0xed, 0x55, 0xca, 0x84, 0xda, 0x51, 0xde,
                              0x40, 0x0b, 0x14, 0x00, 0x27, 0x38, 0x00, 0xd3};
    const uint8_t v513[16] = {0xda, 0x35, 0x14, 0x54, 0x9f, 0x41, 0x6d, 0x11,
                              0xbe, 0x2a, 0xdc, 0x6e, 0x51, 0x14, 0x68, 0x25};
    const uint8_t v002[16] = {0x47, 0x95, 0xc5, 0x1d, 0x74, 0x6b, 0xa0, 0x97,
                              0xd0, 0x28, 0x5a, 0x34, 0xde, 0x8c, 0xca, 0xd3};
    const uint8_t v038[16] = {0x71, 0x5f, 0xd2, 0x2f, 0xd6, 0x6b, 0x47, 0xda,
                              0x3d, 0x94, 0x1b, 0x65, 0x85, 0x59, 0x06, 0x67};

    check_c18_native_bag_layout_candidate(v507, "v507", true);
    check_c18_native_bag_layout_candidate(v508, "v508", true);
    check_c18_native_bag_layout_candidate(v512, "v512", true);
    check_c18_native_bag_layout_candidate(v513, "v513", true);
    check_c18_native_bag_layout_candidate(v002, "v0.2", false);
    check_c18_native_bag_layout_candidate(v038, "v0.38", false);
}

static void test_c18_signature_detection_and_unknown_fallback() {
    const std::string paired = "prefix C18BAG01 asset C18Lbag.dat suffix";
    const std::string magic_only = "prefix C18BAG01 suffix";
    const std::string data_only = "prefix C18Lbag.dat suffix";
    const std::string other_data = "prefix C18BAG01 unrelated.dat suffix";
    const auto bytes = [](const std::string& text) {
        return reinterpret_cast<const uint8_t*>(text.data());
    };

    CHECK(qol::has_c18_native_bag_layout_candidate_signature(bytes(paired), paired.size()));
    CHECK(!qol::has_c18_native_bag_layout_candidate_signature(bytes(magic_only), magic_only.size()));
    CHECK(!qol::has_c18_native_bag_layout_candidate_signature(bytes(data_only), data_only.size()));
    CHECK(!qol::has_c18_native_bag_layout_candidate_signature(bytes(other_data), other_data.size()));
    CHECK(!qol::has_c18_native_bag_layout_candidate_signature(nullptr, paired.size()));

    const uint8_t unknown_md5[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                     0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
    CHECK(qol::game_variant_detail::lookup(unknown_md5) == nullptr);
    qol::RuntimeMarkers unknown_markers{};
    unknown_markers.has_c18_bag_marker = true;
    unknown_markers.has_c18_bag_data_marker = true;
    const qol::GameVariant fallback = qol::game_variant_detail::fallback_variant(unknown_md5, unknown_markers);
    CHECK(fallback.series == qol::GameSeries::kUnknown);
    CHECK((fallback.capabilities & qol::kCapC18NativeBagLayoutCandidate) != 0);
    CHECK(!fallback.known);
    CHECK(std::strcmp(fallback.version_label, "") == 0);
    CHECK(std::memcmp(fallback.md5, unknown_md5, sizeof(unknown_md5)) == 0);
    uint32_t sell_num = 0;
    uint32_t sell_den = 0;
    CHECK(qol::sell_multiplier(fallback.series, qol::contracts::kDetailSell, &sell_num, &sell_den));
    CHECK(sell_num == 0 && sell_den == 10);

    const uint8_t no_md5[16] = {};
    const qol::GameVariant no_md5_fallback =
        qol::game_variant_detail::fallback_variant(no_md5, unknown_markers);
    CHECK(no_md5_fallback.series == qol::GameSeries::kUnknown);
    CHECK((no_md5_fallback.capabilities & qol::kCapC18NativeBagLayoutCandidate) != 0);
    CHECK(!no_md5_fallback.known);
    CHECK(std::strcmp(no_md5_fallback.version_label, "") == 0);
    CHECK(std::memcmp(no_md5_fallback.md5, no_md5, sizeof(no_md5)) == 0);

    qol::RuntimeMarkers conflicted_markers = unknown_markers;
    conflicted_markers.has_wh4_marker = true;
    const qol::GameVariant conflicted =
        qol::game_variant_detail::fallback_variant(no_md5, conflicted_markers);
    CHECK(conflicted.series == qol::GameSeries::kUnknown);
    CHECK((conflicted.capabilities & qol::kCapC18NativeBagLayoutCandidate) == 0);
    CHECK(!conflicted.known);

    conflicted_markers.has_wh4_executable_marker = true;
    const qol::GameVariant executable_conflict =
        qol::game_variant_detail::fallback_variant(unknown_md5, conflicted_markers);
    CHECK(executable_conflict.series == qol::GameSeries::kMonster);
    CHECK((executable_conflict.capabilities & qol::kCapC18NativeBagLayoutCandidate) == 0);
    CHECK(!executable_conflict.known);
}

struct MappingFixture {
    std::vector<std::string> data;
    std::vector<std::string> permissions;
    std::vector<std::string> paths;
    int scan_calls = 0;

    void add(std::string bytes, std::string perms, std::string path) {
        data.push_back(std::move(bytes));
        permissions.push_back(std::move(perms));
        paths.push_back(std::move(path));
    }
};

static qol::RuntimeMarkers scan_mapping_fixture(void* context) {
    auto* fixture = static_cast<MappingFixture*>(context);
    ++fixture->scan_calls;
    qol::RuntimeMarkers markers{};
    for (size_t i = 0; i < fixture->data.size(); ++i) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(fixture->data[i].data());
        const qol::MemoryMappingView mapping{bytes, fixture->data[i].size(),
                                             fixture->permissions[i].c_str(),
                                             fixture->paths[i].c_str()};
        qol::collect_mapping_markers(mapping, markers);
    }
    return markers;
}

static qol::GameVariant resolve_with_mapping_fixture(const uint8_t md5[16], bool have_md5,
                                                     MappingFixture& fixture) {
    return qol::game_variant_detail::resolve_variant(md5, have_md5, scan_mapping_fixture, &fixture);
}

static void test_runtime_mapping_fixture_filters_and_table_authority() {
    constexpr const char* kApkLibgamePath =
        "/data/app/~~fixture/package/base.apk!/lib/arm64-v8a/libgame.so";
    const uint8_t unknown_md5[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                     0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
    const uint8_t no_md5[16] = {};
    const std::string paired = "C18BAG01 C18Lbag.dat";

    MappingFixture archive_libgame{};
    archive_libgame.add(paired, "r--p", kApkLibgamePath);
    const qol::GameVariant candidate = resolve_with_mapping_fixture(unknown_md5, true, archive_libgame);
    CHECK(archive_libgame.scan_calls == 1);
    CHECK(candidate.series == qol::GameSeries::kUnknown);
    CHECK((candidate.capabilities & qol::kCapC18NativeBagLayoutCandidate) != 0);
    CHECK(!candidate.known);
    CHECK(std::strcmp(candidate.version_label, "") == 0);

    MappingFixture archive_no_md5{};
    archive_no_md5.add(paired, "r--p", kApkLibgamePath);
    const qol::GameVariant no_md5_candidate = resolve_with_mapping_fixture(no_md5, false, archive_no_md5);
    CHECK(archive_no_md5.scan_calls == 1);
    CHECK(no_md5_candidate.series == qol::GameSeries::kUnknown);
    CHECK((no_md5_candidate.capabilities & qol::kCapC18NativeBagLayoutCandidate) != 0);
    CHECK(!no_md5_candidate.known);
    CHECK(std::strcmp(no_md5_candidate.version_label, "") == 0);

    MappingFixture unrelated_library{};
    unrelated_library.add(paired, "r--p", "/data/app/package/lib/arm64-v8a/libother.so");
    const qol::GameVariant unrelated = resolve_with_mapping_fixture(unknown_md5, true, unrelated_library);
    CHECK(unrelated_library.scan_calls == 1);
    CHECK((unrelated.capabilities & qol::kCapC18NativeBagLayoutCandidate) == 0);

    MappingFixture unreadable_libgame{};
    unreadable_libgame.add(paired, "---p", kApkLibgamePath);
    const qol::GameVariant unreadable = resolve_with_mapping_fixture(unknown_md5, true, unreadable_libgame);
    CHECK((unreadable.capabilities & qol::kCapC18NativeBagLayoutCandidate) == 0);

    MappingFixture partial_signature{};
    partial_signature.add("C18BAG01", "r--p", kApkLibgamePath);
    const qol::GameVariant partial = resolve_with_mapping_fixture(unknown_md5, true, partial_signature);
    CHECK((partial.capabilities & qol::kCapC18NativeBagLayoutCandidate) == 0);

    MappingFixture wh4_conflict{};
    wh4_conflict.add("C18BAG01 C18Lbag.dat WH4JRN01", "r-xp", kApkLibgamePath);
    const qol::GameVariant conflict = resolve_with_mapping_fixture(unknown_md5, true, wh4_conflict);
    CHECK(conflict.series == qol::GameSeries::kMonster);
    CHECK((conflict.capabilities & qol::kCapC18NativeBagLayoutCandidate) == 0);

    const uint8_t old_c18_md5[16] = {0x47, 0x95, 0xc5, 0x1d, 0x74, 0x6b, 0xa0, 0x97,
                                     0xd0, 0x28, 0x5a, 0x34, 0xde, 0x8c, 0xca, 0xd3};
    MappingFixture known_table_hit{};
    known_table_hit.add(paired, "r--p", kApkLibgamePath);
    const qol::GameVariant known = resolve_with_mapping_fixture(old_c18_md5, true, known_table_hit);
    CHECK(known_table_hit.scan_calls == 0);
    CHECK(known.series == qol::GameSeries::kC18);
    CHECK(std::strcmp(known.version_label, "v0.2") == 0);
    CHECK(known.capabilities == 0);
    CHECK(known.known);
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
    CHECK(std::strcmp(qol::game_series_name(qol::GameSeries::kC18), "c18") == 0);
}

static void test_sell_multipliers() {
    struct Expected {
        qol::GameSeries series;
        uint32_t num;
        uint32_t den;
    };
    constexpr Expected kDetail[] = {
        {qol::GameSeries::kOriginal, 0, 10},
        {qol::GameSeries::kOverhaul, 7, 10},
        {qol::GameSeries::kMonster, 7, 10},
        {qol::GameSeries::kC18, 17, 20},
        {qol::GameSeries::kUnknown, 0, 10},
    };
    for (const Expected& expected : kDetail) {
        uint32_t num = 0;
        uint32_t den = 0;
        CHECK(qol::sell_multiplier(expected.series, qol::contracts::kDetailSell, &num, &den));
        CHECK(num == expected.num && den == expected.den);
        CHECK(qol::sell_multiplier(expected.series, qol::contracts::kStoreSell, &num, &den));
        CHECK(num == 10 && den == 10);
    }
    uint32_t num = 0;
    uint32_t den = 0;
    CHECK(!qol::sell_multiplier(qol::GameSeries::kMonster, 2, &num, &den));
    CHECK(!qol::sell_multiplier(qol::GameSeries::kMonster, qol::contracts::kDetailSell,
                                nullptr, &den));
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
    test_lookup_c18_entry();
    test_lookup_c18_native_bag_layout_candidate();
    test_c18_signature_detection_and_unknown_fallback();
    test_runtime_mapping_fixture_filters_and_table_authority();
    test_lookup_miss();
    test_classify_fallback();
    test_series_names();
    test_sell_multipliers();
    test_uninitialized_default();
    std::printf("game_variant_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
