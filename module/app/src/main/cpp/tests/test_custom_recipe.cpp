#include "feature/custom_recipe/custom_recipe_catalog.h"
#include "feature/custom_recipe/custom_recipe_rules.h"
#include "feature/custom_recipe/custom_recipe_table.h"

#include "feature/attribute_range/attribute_range.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using attr_range::classify;
using attr_range::Tier;
namespace cr = custom_recipe;

static uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

// ---------------------------------------------------------------------------
// §5.1(1) 档位区间边界 + 严格互逆：对每个 (min,max) 与每档，区间端点归属正确。
// ---------------------------------------------------------------------------
static void test_interval_inverse_boundaries() {
    // 退化（max<=min）恒金档、不可升。
    CHECK(cr::compute_tier_up_value(5, 10, 10, nullptr, nullptr, nullptr) ==
          cr::UpgradeResult::kDegenerate);
    CHECK(cr::compute_tier_up_value(0, 0, 0, nullptr, nullptr, nullptr) ==
          cr::UpgradeResult::kDegenerate);

    // 穷举小范围：对每个非金档 value，next_tier_interval 得到的 [lo,hi] 内所有值
    // classify 恒等于目标档，且端点外侧不属于目标档（严格互逆，无浮点漂移）。
    const int min = 0;
    const int max = 100;  // R=100，六档均有整数代表值
    int seen_tiers = 0;
    for (int v = min; v < max; ++v) {
        const Tier cur = classify(v, min, max);
        if (cur == Tier::Gold) continue;
        Tier target = Tier::Grey;
        int lo = 0;
        int hi = 0;
        if (!cr::next_tier_interval(v, min, max, &target, &lo, &hi)) {
            ++g_fail;
            std::printf("FAIL interval missing for v=%d\n", v);
            continue;
        }
        ++seen_tiers;
        // 目标档严格高于当前档。
        CHECK(cr::tier_ordinal(target) == cr::tier_ordinal(cur) + 1);
        // 区间内每个值都判为目标档（与 classify 互逆）。
        CHECK(classify(lo, min, max) == target);
        CHECK(classify(hi, min, max) == target);
        for (int x = lo; x <= hi; ++x) {
            if (classify(x, min, max) != target) {
                ++g_fail;
                std::printf("FAIL interval leak x=%d target=%d got=%d\n", x,
                            cr::tier_ordinal(target), cr::tier_ordinal(classify(x, min, max)));
                break;
            }
        }
        // 端点外侧（在 [min,max] 内）不得属于目标档。
        if (lo > min) CHECK(classify(lo - 1, min, max) != target);
        if (hi < max) CHECK(classify(hi + 1, min, max) != target);
    }
    CHECK(seen_tiers > 0);

    // 金档不可升（v>=max）。
    CHECK(cr::compute_tier_up_value(100, 0, 100, nullptr, nullptr, nullptr) ==
          cr::UpgradeResult::kAlreadyGold);
    CHECK(cr::compute_tier_up_value(150, 0, 100, nullptr, nullptr, nullptr) ==
          cr::UpgradeResult::kAlreadyGold);
}

// 固定返回下界的随机源（可预测）。
static int rand_lo(int lo, int hi) { (void)hi; return lo; }
// 固定返回上界的随机源。
static int rand_hi(int lo, int hi) { (void)lo; return hi; }
// 覆盖式：每次调用返回区间内递增位置（由外部游标驱动）。
static int g_rand_cursor = 0;
static int rand_sweep(int lo, int hi) {
    const int span = hi - lo;
    const int idx = g_rand_cursor % (span + 1);
    return lo + idx;
}

// ---------------------------------------------------------------------------
// §5.1(2) 随机取值后 classify(new)==目标档（含边界源 + 多轮扫掠）。
// ---------------------------------------------------------------------------
static void test_random_matches_target() {
    struct Case { int value; int min; int max; };
    const Case cases[] = {
        {0, 0, 100}, {15, 0, 100}, {30, 0, 100}, {50, 0, 100},
        {70, 0, 100}, {80, 0, 100}, {95, 0, 100}, {42, 10, 200},
    };
    for (const Case& c : cases) {
        const Tier cur = classify(c.value, c.min, c.max);
        if (cur == Tier::Gold) continue;
        for (int (*rand)(int, int) : {rand_lo, rand_hi}) {
            Tier target = Tier::Grey;
            int out = 0;
            const auto r = cr::compute_tier_up_value(c.value, c.min, c.max, rand, &target, &out);
            CHECK(r == cr::UpgradeResult::kOk);
            CHECK(classify(out, c.min, c.max) == target);
            CHECK(cr::tier_ordinal(target) == cr::tier_ordinal(cur) + 1);
        }
        // 扫掠区间内每个可能取值都必须判为目标档。
        Tier target = Tier::Grey;
        int lo = 0;
        int hi = 0;
        CHECK(cr::next_tier_interval(c.value, c.min, c.max, &target, &lo, &hi));
        for (int i = 0; i <= hi - lo; ++i) {
            g_rand_cursor = i;
            int out = 0;
            Tier t2 = Tier::Grey;
            CHECK(cr::compute_tier_up_value(c.value, c.min, c.max, rand_sweep, &t2, &out) ==
                  cr::UpgradeResult::kOk);
            CHECK(out >= lo && out <= hi);
            CHECK(t2 == target);
            CHECK(classify(out, c.min, c.max) == target);
        }
    }
    g_rand_cursor = 0;
}

// ---------------------------------------------------------------------------
// §5.1(3) 目录映射：N=1（真实目录）与合成 N=3。
// ---------------------------------------------------------------------------
static void test_catalog_mapping() {
    size_t n = 0;
    const cr::Def* real = cr::catalog(&n);
    CHECK(n == 1);
    CHECK(real != nullptr);
    CHECK(real[0].material_count == 1);
    CHECK(real[0].materials[0].item_id == 15);
    CHECK(real[0].kind == cr::Kind::kJewelTierUp);

    const uint16_t base_rec = 69;
    const uint16_t base_mat = 189;
    CHECK(cr::mix_type_at(base_rec, 0) == 69);
    CHECK(cr::material_start_at(base_mat, real, n, 0) == 189);
    CHECK(cr::material_total(real, n) == 1);
    CHECK(real[0].count_rule == cr::CountRule::kJewelGradeAndLevel);

    // 合成 3 条目录：材料条目数分别 1 / 2 / 3。
    const cr::Material m1[] = {{15, 1}};
    const cr::Material m2[] = {{15, 1}, {16, 2}};
    const cr::Material m3[] = {{15, 1}, {16, 1}, {17, 1}};
    const cr::Def synth[3] = {
        {35291, cr::Kind::kJewelTierUp, m1, 1, 0, cr::CountRule::kFixed},
        {35292, cr::Kind::kJewelTierUp, m2, 2, 0, cr::CountRule::kFixed},
        {35293, cr::Kind::kJewelTierUp, m3, 3, 0, cr::CountRule::kFixed},
    };
    CHECK(cr::mix_type_at(base_rec, 0) == 69);
    CHECK(cr::mix_type_at(base_rec, 1) == 70);
    CHECK(cr::mix_type_at(base_rec, 2) == 71);
    CHECK(cr::material_start_at(base_mat, synth, 3, 0) == 189);
    CHECK(cr::material_start_at(base_mat, synth, 3, 1) == 190);  // 189 + 1
    CHECK(cr::material_start_at(base_mat, synth, 3, 2) == 192);  // 189 + 1 + 2
    CHECK(cr::material_total(synth, 3) == 6);
}

// ---------------------------------------------------------------------------
// §5.1(4) 表注入字节断言（伪造原表，不依赖游戏内存）。
// ---------------------------------------------------------------------------
static void test_inject_bytes() {
    // 伪造 RECIPEBASE：3 条原版记录，12B/条。材料引用尾部最大 = record2: b4-5=10,b6=4 → 14。
    uint8_t orig_recipe[3 * cr::kRecipeRecordSize];
    std::memset(orig_recipe, 0, sizeof(orig_recipe));
    auto put = [&](uint8_t* rec, size_t off, uint16_t v) {
        rec[off] = static_cast<uint8_t>(v & 0xff);
        rec[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    // record0: start=0 count=4 → end 4
    put(orig_recipe + 0, cr::kRbMaterialStart, 0);
    orig_recipe[0 * cr::kRecipeRecordSize + cr::kRbMaterialCount] = 4;
    // record1: start=4 count=3 → end 7
    put(orig_recipe + 1 * cr::kRecipeRecordSize, cr::kRbMaterialStart, 4);
    orig_recipe[1 * cr::kRecipeRecordSize + cr::kRbMaterialCount] = 3;
    // record2: start=10 count=4 → end 14 (max)
    put(orig_recipe + 2 * cr::kRecipeRecordSize, cr::kRbMaterialStart, 10);
    orig_recipe[2 * cr::kRecipeRecordSize + cr::kRbMaterialCount] = 4;
    // 伪造 MIXTUREBASE：14 条 3B 材料，填可辨识字节 0xA0+idx。
    uint8_t orig_mixture[14 * cr::kMixtureRecordSize];
    for (size_t i = 0; i < 14; ++i) {
        std::memset(orig_mixture + i * cr::kMixtureRecordSize, static_cast<int>(0xA0 + i),
                    cr::kMixtureRecordSize);
    }

    CHECK(cr::derive_material_count(orig_recipe, 3, cr::kRecipeRecordSize) == 14);

    size_t n = 0;
    const cr::Def* cat = cr::catalog(&n);  // N=1，材料 1 条 {15,1}

    uint8_t out_recipe[4 * cr::kRecipeRecordSize];
    uint8_t out_mixture[(14 + 1) * cr::kMixtureRecordSize];
    std::memset(out_recipe, 0, sizeof(out_recipe));
    std::memset(out_mixture, 0, sizeof(out_mixture));

    const uint32_t result =
        cr::inject_into_buffers(orig_recipe, 3, cr::kRecipeRecordSize, orig_mixture,
                                cr::kMixtureRecordSize, cat, n, out_recipe, out_mixture);
    CHECK(result == 4);  // base 3 + N 1

    // 前段原样复制。
    CHECK(std::memcmp(out_recipe, orig_recipe, sizeof(orig_recipe)) == 0);
    CHECK(std::memcmp(out_mixture, orig_mixture, sizeof(orig_mixture)) == 0);

    // 注入记录字段逐个正确。
    const uint8_t* inj = out_recipe + 3 * cr::kRecipeRecordSize;
    CHECK(rd_u16(inj + cr::kRbLabel) == 35291);
    CHECK(rd_u16(inj + cr::kRbResultId) == 0);
    CHECK(rd_u16(inj + cr::kRbMaterialStart) == 14);       // base_material_count
    CHECK(inj[cr::kRbMaterialCount] == 1);
    CHECK(inj[cr::kRbFlag7] == 1);
    CHECK(rd_u16(inj + cr::kRbCostWord) == 0);
    CHECK(inj[cr::kRbUnlockGate] == 0);                     // 必须 0（§7.8）
    CHECK(inj[cr::kRbGroup] == 0x02);                       // bit1 组，bit0/bit5 清

    // 追加材料条目：MIXTUREBASE[14] = {15,1}。
    const uint8_t* mat = out_mixture + 14 * cr::kMixtureRecordSize;
    CHECK(rd_u16(mat) == 15);
    CHECK(mat[2] == 1);

    // 幂等：相同输入重复调用产生完全相同的输出。
    uint8_t out_recipe2[4 * cr::kRecipeRecordSize];
    uint8_t out_mixture2[(14 + 1) * cr::kMixtureRecordSize];
    const uint32_t result2 =
        cr::inject_into_buffers(orig_recipe, 3, cr::kRecipeRecordSize, orig_mixture,
                                cr::kMixtureRecordSize, cat, n, out_recipe2, out_mixture2);
    CHECK(result2 == result);
    CHECK(std::memcmp(out_recipe, out_recipe2, sizeof(out_recipe)) == 0);
    CHECK(std::memcmp(out_mixture, out_mixture2, sizeof(out_mixture)) == 0);
}

// ---------------------------------------------------------------------------
// §5.1(5) 材料需求数量：宝石档位映射 + ceil(档位 × (105 − 等级) / 105) 边界与单调性。
// ---------------------------------------------------------------------------
static void test_material_count() {
    // 类别 → 档位：28=低级宝石 … 32=混沌宝石；越界一律 0（调用方据此放弃改写需求数）。
    CHECK(cr::jewel_grade_from_category(28) == 1);
    CHECK(cr::jewel_grade_from_category(29) == 2);
    CHECK(cr::jewel_grade_from_category(30) == 3);
    CHECK(cr::jewel_grade_from_category(31) == 4);
    CHECK(cr::jewel_grade_from_category(32) == 5);
    CHECK(cr::jewel_grade_from_category(27) == 0);
    CHECK(cr::jewel_grade_from_category(33) == 0);
    CHECK(cr::jewel_grade_from_category(0) == 0);
    CHECK(cr::jewel_grade_from_category(-1) == 0);
    CHECK(cr::kJewelCategoryFirst == 28 && cr::kJewelCategoryLast == 32);

    // 满级 105 → 0 瓶（idea.md §7：满级后无需消耗卓越灵药）；超上限同样 0，不得为负。
    for (int cat = cr::kJewelCategoryFirst; cat <= cr::kJewelCategoryLast; ++cat) {
        const int grade = cr::jewel_grade_from_category(cat);
        CHECK(cr::material_count_for(1, grade, 105) == 0);
        CHECK(cr::material_count_for(3, grade, 105) == 0);
    }
    CHECK(cr::material_count_for(1, 5, 200) == 0);

    // 低等级 ≈ 原量：grade 1..5 对应 1..5 瓶（低级1瓶、中级2瓶、类推）。
    for (int g = 1; g <= 5; ++g) {
        CHECK(cr::material_count_for(1, g, 1) == g);
        CHECK(cr::material_count_for(1, g, 0) == g);
        CHECK(cr::material_count_for(1, g, -5) == g);  // 负等级钳到 0
    }
    CHECK(cr::material_count_for(2, 1, 1) == 2);  // base_count 作倍率
    CHECK(cr::material_count_for(3, 5, 105) == 0);

    // 向上取整：ceil(5 × 53/105) = ceil(2.52…) = 3；ceil(5 × 52/105) = 3。
    CHECK(cr::material_count_for(1, 5, 52) == 3);
    CHECK(cr::material_count_for(1, 5, 53) == 3);
    // 任何非零需求不得被取整成 0：ceil(1 × 104/105) = 1、ceil(1 × 52/105) = 1。
    CHECK(cr::material_count_for(1, 1, 1) == 1);
    CHECK(cr::material_count_for(1, 1, 53) == 1);

    // 非法输入 → 0（调用方据此保持原需求数，不会变成免费合成）。
    CHECK(cr::material_count_for(0, 5, 0) == 0);
    CHECK(cr::material_count_for(-1, 1, 0) == 0);
    CHECK(cr::material_count_for(1, 0, 0) == 0);

    // 对等级单调不增，且始终落在 [0, base×grade]。
    for (int g = 1; g <= 5; ++g) {
        int prev = cr::material_count_for(1, g, 0);
        CHECK(prev == g);
        for (int lv = 1; lv <= cr::kMaxCharacterLevel; ++lv) {
            const int cur = cr::material_count_for(1, g, lv);
            CHECK(cur <= prev);
            CHECK(cur >= 0);
            prev = cur;
        }
        CHECK(prev == 0);
    }
}

int main() {
    test_interval_inverse_boundaries();
    test_random_matches_target();
    test_catalog_mapping();
    test_inject_bytes();
    test_material_count();
    std::printf("custom_recipe_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
