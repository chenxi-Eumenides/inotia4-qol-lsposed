#include "feature/attribute_range/attribute_range.h"

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using attr_range::classify;
using attr_range::color_code;
using attr_range::Tier;

static void test_bounds() {
    // value == max -> Gold；value > max -> Gold。
    CHECK(classify(100, 0, 100) == Tier::Gold);
    CHECK(classify(101, 0, 100) == Tier::Gold);
    CHECK(classify(1000, 0, 100) == Tier::Gold);
    // max <= min -> Gold（退化区间）。
    CHECK(classify(0, 10, 10) == Tier::Gold);
    CHECK(classify(5, 10, 5) == Tier::Gold);
    CHECK(classify(5, 10, 0) == Tier::Gold);
}

static void test_percentile_thresholds() {
    // 恰好 90% -> Purple；89% -> Blue。
    CHECK(classify(90, 0, 100) == Tier::Purple);
    CHECK(classify(89, 0, 100) == Tier::Blue);
    // 75% -> Blue；74% -> Green。
    CHECK(classify(75, 0, 100) == Tier::Blue);
    CHECK(classify(74, 0, 100) == Tier::Green);
    // 60% -> Green；59% -> White。
    CHECK(classify(60, 0, 100) == Tier::Green);
    CHECK(classify(59, 0, 100) == Tier::White);
    // 30% -> White；29% -> Grey。
    CHECK(classify(30, 0, 100) == Tier::White);
    CHECK(classify(29, 0, 100) == Tier::Grey);
    // value == min -> Grey；value < min -> Grey。
    CHECK(classify(0, 0, 100) == Tier::Grey);
    CHECK(classify(-1, 0, 100) == Tier::Grey);
    CHECK(classify(-1000, 0, 100) == Tier::Grey);
}

static void test_color_code() {
    CHECK(color_code(Tier::Grey) == 'G');
    CHECK(color_code(Tier::White) == 'W');
    CHECK(color_code(Tier::Green) == 'C');
    CHECK(color_code(Tier::Blue) == 'L');
    CHECK(color_code(Tier::Purple) == 'V');
    CHECK(color_code(Tier::Gold) == 'Y');
}

static void test_nonzero_min() {
    // min=5, max=10：value=10 -> Gold；value=5 -> Grey；value=8 -> pct=60 -> Green。
    CHECK(classify(10, 5, 10) == Tier::Gold);
    CHECK(classify(5, 5, 10) == Tier::Grey);
    CHECK(classify(8, 5, 10) == Tier::Green);
    // 区间内其余档位抽查：6 -> 20% -> Grey；7 -> 40% -> White；9 -> 80% -> Blue。
    CHECK(classify(6, 5, 10) == Tier::Grey);
    CHECK(classify(7, 5, 10) == Tier::White);
    CHECK(classify(9, 5, 10) == Tier::Blue);
}

static void test_no_int_overflow() {
    // (value - min) * 100 需用 64 位：max-min 仍在 int 范围内，但 numerator 超过
    // INT_MAX 时不得溢出误判档位。
    const int int_max = 2147483647;
    // min=0, max=INT_MAX, value=INT_MAX-1 -> 接近 100% -> Purple。
    CHECK(classify(int_max - 1, 0, int_max) == Tier::Purple);
    // min=0, max=1e8, value=99999999 -> numerator 9,999,999,900 溢出 int，
    // 正确 pct≈99 -> Purple。
    CHECK(classify(99999999, 0, 100000000) == Tier::Purple);
    // 同一大区间中点 -> pct=50 -> White。
    CHECK(classify(50000000, 0, 100000000) == Tier::White);
    // 非零 min 大区间：min=5e7, max=1e8, value=8e7 -> pct=60 -> Green。
    CHECK(classify(80000000, 50000000, 100000000) == Tier::Green);
}

int main() {
    test_bounds();
    test_percentile_thresholds();
    test_color_code();
    test_nonzero_min();
    test_no_int_overflow();
    std::printf("attribute_range_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
