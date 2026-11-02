// 商店扩展物品出售计数/计价判定纯函数测试（qol::store_sell::plan）。
// 覆盖：非 encoded / view<=1 整件、encoded 部分整除、越界/不整除/非正值拒绝，
// 以及 canonical 199（a=1,b=71）与关闭态 b 视图的边界。

#include "contracts/store_sell_rules.h"

#include <cstdint>
#include <cstdio>

using qol::store_sell::Plan;
using qol::store_sell::PlanKind;
using qol::store_sell::plan;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_whole_paths() {
    // 非 count-encoded：原版卖 1 件。
    CHECK(plan(false, 100, 500, 500).kind == PlanKind::kWhole);
    CHECK(plan(false, 1, 500, 500).count == 1);
    // view<=1：无数量面板，整件。
    CHECK(plan(true, 0, 500, 500).kind == PlanKind::kWhole);
    CHECK(plan(true, 1, 500, 500).kind == PlanKind::kWhole);
    CHECK(plan(true, 1, 500, 0).kind == PlanKind::kWhole);
    CHECK(plan(true, -1, 500, 500).kind == PlanKind::kWhole);
}

static void test_partial_paths() {
    // canonical 199，单价 7：整堆价 1393、卖 150 价 1050。
    CHECK(plan(true, 199, 7, 1393).kind == PlanKind::kPartial);
    CHECK(plan(true, 199, 7, 1393).count == 199);
    CHECK(plan(true, 199, 7, 1050).count == 150);
    CHECK(plan(true, 199, 7, 7).count == 1);
    // 关闭态 b 视图（199 → 71）：上限为 71。
    CHECK(plan(true, 71, 7, 71 * 7).count == 71);
    CHECK(plan(true, 71, 7, 72 * 7).kind == PlanKind::kReject);
    // 单价 1 / 大数量。
    CHECK(plan(true, 999, 1, 999).count == 999);
    CHECK(plan(true, 999, 1, 1000).kind == PlanKind::kReject);
}

static void test_reject_paths() {
    // 不整除。
    CHECK(plan(true, 199, 7, 1394).kind == PlanKind::kReject);
    // 单价非正 / 载荷非正。
    CHECK(plan(true, 199, 0, 1393).kind == PlanKind::kReject);
    CHECK(plan(true, 199, -7, 1393).kind == PlanKind::kReject);
    CHECK(plan(true, 199, 7, 0).kind == PlanKind::kReject);
    CHECK(plan(true, 199, 7, -7).kind == PlanKind::kReject);
    // count 越界（0 或 > view）。
    CHECK(plan(true, 199, 7, 0).kind == PlanKind::kReject);
    CHECK(plan(true, 199, 1000, 0).kind == PlanKind::kReject);
    CHECK(plan(true, 2, 7, 7 * 3).kind == PlanKind::kReject);
}

int main() {
    test_whole_paths();
    test_partial_paths();
    test_reject_paths();
    std::printf("store_sell_rules: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
