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
using attr_range::percentile;
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

static void test_percentile_bounds() {
    // max <= min -> 100（退化区间视为满值）。
    CHECK(percentile(0, 10, 10) == 100);
    CHECK(percentile(5, 10, 5) == 100);
    CHECK(percentile(5, 10, 0) == 100);
    CHECK(percentile(0, 0, 0) == 100);

    // value >= max -> 100（含超出范围的特殊/固定值）。
    CHECK(percentile(100, 0, 100) == 100);
    CHECK(percentile(101, 0, 100) == 100);
    CHECK(percentile(1000, 0, 100) == 100);
    CHECK(percentile(10, 5, 10) == 100);

    // value < min -> 0（不得为负）。
    CHECK(percentile(-1, 0, 100) == 0);
    CHECK(percentile(-1000, 0, 100) == 0);
    CHECK(percentile(4, 5, 10) == 0);
    CHECK(percentile(0, 5, 10) == 0);
}

static void test_percentile_values() {
    // 分档阈值 30/60/75/90 及 0/100 端点的百分位定义（min=0,max=100）。
    CHECK(percentile(0, 0, 100) == 0);
    CHECK(percentile(29, 0, 100) == 29);
    CHECK(percentile(30, 0, 100) == 30);
    CHECK(percentile(59, 0, 100) == 59);
    CHECK(percentile(60, 0, 100) == 60);
    CHECK(percentile(74, 0, 100) == 74);
    CHECK(percentile(75, 0, 100) == 75);
    CHECK(percentile(89, 0, 100) == 89);
    CHECK(percentile(90, 0, 100) == 90);
    CHECK(percentile(99, 0, 100) == 99);

    // 向下取整：1/3 -> 33%，2/3 -> 66%。
    CHECK(percentile(1, 0, 3) == 33);
    CHECK(percentile(2, 0, 3) == 66);

    // 非零 min：min=5,max=10 -> 8 => (8-5)*100/5 = 60。
    CHECK(percentile(5, 5, 10) == 0);
    CHECK(percentile(6, 5, 10) == 20);
    CHECK(percentile(7, 5, 10) == 40);
    CHECK(percentile(8, 5, 10) == 60);
    CHECK(percentile(9, 5, 10) == 80);

    // 大区间仍用 64 位避免溢出：接近上界 -> 99。
    CHECK(percentile(99999999, 0, 100000000) == 99);
    CHECK(percentile(50000000, 0, 100000000) == 50);
    CHECK(percentile(80000000, 50000000, 100000000) == 60);
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
    test_percentile_bounds();
    test_percentile_values();
    test_color_code();
    test_nonzero_min();
    test_no_int_overflow();
    std::printf("attribute_range_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
