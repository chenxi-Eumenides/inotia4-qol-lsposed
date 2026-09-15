#include "feature/ui/game_ui_charinfo_zh.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using charinfo_zh::Entry;
using charinfo_zh::kEntries;
using charinfo_zh::kEntryCount;
using charinfo_zh::lookup;

// 期望白名单（用户裁决的唯一事实对照）：id -> 中文。
struct Expected {
    uint16_t id;
    const char* zh;
};

static constexpr Expected kExpected[] = {
    {35185, "物攻"},    // DMG   物理攻击力
    {35186, "法攻"},    // M. DMG 魔法攻击力
    {35187, "防御力"},  // DEF   防御力
    {35189, "暴击率"},  // CRT   暴击率
    {35190, "命中率"},  // H.RATE 命中率
    {35191, "爆伤"},    // C.DMG 暴击伤害（两字：用户裁决优于「暴击伤」）
    {35192, "物抗"},    // P.RES 物理抗性（修正早前的错误标签「毒抗」）
    {35193, "法抗"},    // M. RES 魔法抗性
    {35194, "闪避率"},  // EVD   闪避率
    {35195, "武器格挡"},  // W.D.R 武器格挡率
    {35196, "盾牌格挡"},  // S.D.R 盾牌格挡率
};
static constexpr size_t kExpectedCount = sizeof(kExpected) / sizeof(kExpected[0]);

// 明确不翻译：35181 LV / 35182 EXP / 35183 HP / 35184 MP（用户裁决）+ 35188 M. DEF（面板未显示）
static constexpr uint16_t kExcluded[] = {35181, 35182, 35183, 35184, 35188};
static constexpr size_t kExcludedCount = sizeof(kExcluded) / sizeof(kExcluded[0]);

static const char* expected_text(uint16_t id) {
    for (size_t i = 0; i < kExpectedCount; ++i) {
        if (kExpected[i].id == id) return kExpected[i].zh;
    }
    return nullptr;
}

// 表内命中项数必须恰为 11，且 id 唯一、文案非空。
static void test_table_shape() {
    int replaced = 0;
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].zh != nullptr) ++replaced;
    }
    CHECK(replaced == 11);
    CHECK(kExpectedCount == 11);

    // id 严格升序（等价于无重复）且与区间端点一致。
    bool ascending = true;
    for (size_t i = 1; i < kEntryCount; ++i) {
        if (kEntries[i].text_id <= kEntries[i - 1].text_id) ascending = false;
    }
    CHECK(ascending);
    CHECK(kEntries[0].text_id == charinfo_zh::kTextIdMin);
    CHECK(kEntries[kEntryCount - 1].text_id == charinfo_zh::kTextIdMax);

    // 每个非空文案必须是**二至四个汉字**的纯中文 UTF-8（即长度上限四字）：整串恰 6 / 9 / 12 字节，
    // 由 3 字节序列组成（CJK 区 U+4E00..U+9FFF 的首字节落在 0xE4..0xE9）。上限来自面板布局（用户裁决）。
    bool texts_ok = true;
    for (size_t i = 0; i < kEntryCount; ++i) {
        const char* zh = kEntries[i].zh;
        if (zh == nullptr) continue;
        const size_t len = std::strlen(zh);
        if (len != 6 && len != 9 && len != 12) texts_ok = false;
        for (size_t b = 0; b + 2 < len; b += 3) {
            const unsigned char c0 = static_cast<unsigned char>(zh[b]);
            const unsigned char c1 = static_cast<unsigned char>(zh[b + 1]);
            const unsigned char c2 = static_cast<unsigned char>(zh[b + 2]);
            if (c0 < 0xE4 || c0 > 0xE9) texts_ok = false;
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) texts_ok = false;
        }
    }
    CHECK(texts_ok);
}

// 表内容与期望白名单逐项一致（含文案与不替换项）。
static void test_table_matches_expected() {
    for (size_t i = 0; i < kEntryCount; ++i) {
        const Entry& e = kEntries[i];
        const char* want = expected_text(e.text_id);
        const bool excluded_entry = (e.zh == nullptr);
        CHECK(excluded_entry == (want == nullptr));
        if (!excluded_entry && want != nullptr) {
            CHECK(std::strcmp(e.zh, want) == 0);
        }
    }
}

// lookup：命中返回中文、区间外与不替换项返回 nullptr。
static void test_lookup_hits() {
    for (size_t i = 0; i < kExpectedCount; ++i) {
        const char* got = lookup(kExpected[i].id);
        CHECK(got != nullptr);
        if (got != nullptr) CHECK(std::strcmp(got, kExpected[i].zh) == 0);
    }
    // 全表扫描：非 nullptr 的 id 集合与期望白名单完全一致（防误加/漏删）。
    int hits = 0;
    for (uint32_t id = 0; id <= 0xFFFF; ++id) {
        if (lookup(static_cast<uint16_t>(id)) != nullptr) ++hits;
    }
    CHECK(hits == 11);
}

static void test_lookup_misses() {
    for (size_t i = 0; i < kExcludedCount; ++i) {
        CHECK(lookup(kExcluded[i]) == nullptr);
    }
    // 区间外与相邻边界。
    CHECK(lookup(0) == nullptr);
    CHECK(lookup(35184) == nullptr);
    CHECK(lookup(35197) == nullptr);
    CHECK(lookup(0xFFFF) == nullptr);
}

int main() {
    test_table_shape();
    test_table_matches_expected();
    test_lookup_hits();
    test_lookup_misses();
    std::printf("charinfo_zh_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
