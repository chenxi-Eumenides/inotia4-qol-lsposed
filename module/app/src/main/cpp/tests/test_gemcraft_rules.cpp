#include "feature/gemcraft/gemcraft_rules.h"

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using gemcraft::first_empty_slot;
using gemcraft::is_jewel_category;

static void test_is_jewel_category() {
    // 宝石档位 28..32（低/中/高/顶/混沌）为真。
    CHECK(is_jewel_category(28));
    CHECK(is_jewel_category(29));
    CHECK(is_jewel_category(30));
    CHECK(is_jewel_category(31));
    CHECK(is_jewel_category(32));
    // 边界外为假。
    CHECK(!is_jewel_category(27));
    CHECK(!is_jewel_category(33));
    CHECK(!is_jewel_category(0));
    CHECK(!is_jewel_category(-1));
}

static void test_first_empty_slot() {
    // 全空 -> 0。
    const bool all_empty[3] = {false, false, false};
    CHECK(first_empty_slot(all_empty, 3) == 0);

    // 中间空 -> 最小空索引。
    const bool middle_empty[3] = {true, false, true};
    CHECK(first_empty_slot(middle_empty, 3) == 1);

    // 仅末位空 -> 2。
    const bool tail_empty[3] = {true, true, false};
    CHECK(first_empty_slot(tail_empty, 3) == 2);

    // 全满 -> -1。
    const bool all_full[3] = {true, true, true};
    CHECK(first_empty_slot(all_full, 3) == -1);

    // 全空但 count 为 0 / 负数 -> -1（越界 count 保护）。
    CHECK(first_empty_slot(all_empty, 0) == -1);
    CHECK(first_empty_slot(all_empty, -1) == -1);

    // 空指针 -> -1。
    CHECK(first_empty_slot(nullptr, 3) == -1);

    // count=1 时只看首格。
    CHECK(first_empty_slot(all_full, 1) == -1);
    CHECK(first_empty_slot(middle_empty, 1) == -1);
    CHECK(first_empty_slot(all_empty, 1) == 0);
}

int main() {
    test_is_jewel_category();
    test_first_empty_slot();
    std::printf("gemcraft_rules_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
