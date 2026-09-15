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
// §5.1(3) 目录映射：N=2（真实目录，两条均挂宝石强化页 group 3）与合成 N=3。
// ---------------------------------------------------------------------------
static void test_catalog_mapping() {
    size_t n = 0;
    const cr::Def* real = cr::catalog(&n);
    CHECK(n == 2);
    CHECK(real != nullptr);
    // 第 1 条「合成」= kThreeSlotCraft（3 格隐式配方，§2.8/§4.12）：材料/费用字段仍镜像原版
    // record 12（低级宝石×3 / 188）仅供该页材料格与费用**显示**保持原样；能否合成与实际产物
    // 由 `kThreeSlotRecipes` 按 3 格内容决定，放料/合成两条路径都不转调原函数。
    CHECK(real[0].label_word_id == 35216);
    CHECK(real[0].kind == cr::Kind::kThreeSlotCraft);
    CHECK(real[0].material_count == 1);
    CHECK(real[0].materials[0].item_id == 28);
    CHECK(real[0].materials[0].count == 3);
    CHECK(real[0].cost_word_id == 188);
    CHECK(real[0].count_rule == cr::CountRule::kFixed);
    CHECK(real[0].form == cr::Form::kMultiInputCreate);
    CHECK(real[0].group == 3);
    // 第 2 条「宝石强化」= kJewelTierUp：借 type 3（目标物 + 材料 → 原地修改）形态。
    CHECK(real[1].label_word_id == 35291);
    CHECK(real[1].kind == cr::Kind::kJewelTierUp);
    CHECK(real[1].material_count == 1);
    CHECK(real[1].materials[0].item_id == 15);
    // 必须填合法公式 wordId（不能为 0）：宝石强化页是 type 1，而 UIMix_ButtonRecipeExe 内部
    // 自带一次 InitMixingState，其 type==1 分支会用本记录 b8-9 求值 CAL_Calculate —— 填 0 会给
    // CAL_Calculate 传空公式并崩溃（真机实证，见设计册 §7.27）。实际费用由放料 hook 强制 0。
    CHECK(real[1].cost_word_id == 188);
    CHECK(real[1].count_rule == cr::CountRule::kJewelGradeAndLevel);
    CHECK(real[1].form == cr::Form::kTargetAndCreate);
    CHECK(real[1].group == 3);
    // b11 组位推导：1 << group；group >= 8 视为非法（不占任何页）。
    CHECK(cr::recipe_group_bit(3) == 0x08);
    CHECK(cr::recipe_group_bit(6) == 0x40);
    CHECK(cr::recipe_group_bit(8) == 0);

    const uint16_t base_rec = 69;
    const uint16_t base_mat = 189;
    CHECK(cr::mix_type_at(base_rec, 0) == 69);
    CHECK(cr::mix_type_at(base_rec, 1) == 70);
    CHECK(cr::material_start_at(base_mat, real, n, 0) == 189);
    CHECK(cr::material_start_at(base_mat, real, n, 1) == 190);  // 189 + 1
    CHECK(cr::material_total(real, n) == 2);
    // host 进程不注入表 → bind 前读回 0（未绑定）。
    CHECK(cr::bound_base_record_count() == 0);

    // bind 后 mixType→Def 按「目录顺序 = 记录下标升序」映射：group 3 配方列表 = [69, 70]。
    cr::bind_base_record_count(base_rec);
    CHECK(cr::catalog_ready());
    CHECK(cr::def_for_mix_type(69) == &real[0]);
    CHECK(cr::def_for_mix_type(70) == &real[1]);
    CHECK(cr::def_for_mix_type(12) == nullptr);  // 原版宝石记录 12..15 不是模块配方
    CHECK(cr::def_for_mix_type(68) == nullptr);
    CHECK(cr::def_for_mix_type(71) == nullptr);  // 越界（base+2 之后）

    // 合成 3 条目录：材料条目数分别 1 / 2 / 3，组位不同以覆盖映射通用性。
    const cr::Material m1[] = {{15, 1}};
    const cr::Material m2[] = {{15, 1}, {16, 2}};
    const cr::Material m3[] = {{15, 1}, {16, 1}, {17, 1}};
    const cr::Def synth[3] = {
        {35291, cr::Kind::kJewelTierUp, m1, 1, 0, cr::CountRule::kFixed,
         cr::Form::kTargetAndCreate, 3},
        {35292, cr::Kind::kNativePassThrough, m2, 2, 0, cr::CountRule::kFixed,
         cr::Form::kConsumeToCreate, 1},
        {35293, cr::Kind::kJewelTierUp, m3, 3, 0, cr::CountRule::kFixed,
         cr::Form::kMultiInputCreate, 3},
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
// §5.1(4) 表注入字节断言（伪造原表，不依赖游戏内存）：
// - 注入后记录数 = base + 2（目录两条），注入记录 b11 = 0x28（group 3 | 配方书位 bit5）；
// - 原版 group-3 记录（模拟 12..15）的 b11 组位被清且**其它字节逐字节不变**；
// - 材料条目按目录顺序追加（「合成」在前、「宝石强化」在后）。
// ---------------------------------------------------------------------------
static void test_inject_bytes() {
    // 伪造 RECIPEBASE：3 条原版记录，12B/条，字段填原版风格值（断言注入「只动 b11」）。
    // record0 模拟宝石记录 12..15：b11=0x08（仅 group 3）；
    // record1 模拟混沌配方：b11=0x03（不含 group 3 位 → 不得被触碰）；
    // record2 模拟带配方书位的宝石记录：b11=0x28（bit5+bit3 → 清后剩 0x20）。
    uint8_t orig_recipe[3 * cr::kRecipeRecordSize];
    std::memset(orig_recipe, 0, sizeof(orig_recipe));
    auto put = [&](uint8_t* rec, size_t off, uint16_t v) {
        rec[off] = static_cast<uint8_t>(v & 0xff);
        rec[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto fill = [&](size_t r, uint16_t label, uint16_t result, uint16_t start, uint8_t count,
                    uint16_t cost, uint8_t unlock, uint8_t group) {
        uint8_t* rec = orig_recipe + r * cr::kRecipeRecordSize;
        put(rec, cr::kRbLabel, label);
        put(rec, cr::kRbResultId, result);
        put(rec, cr::kRbMaterialStart, start);
        rec[cr::kRbMaterialCount] = count;
        rec[cr::kRbFlag7] = 1;
        put(rec, cr::kRbCostWord, cost);
        rec[cr::kRbUnlockGate] = unlock;
        rec[cr::kRbGroup] = group;
    };
    // record0: start=0 count=4 → end 4；record1: start=4 count=3 → end 7；
    // record2: start=10 count=4 → end 14（材料尾部最大值）。
    fill(0, 1163, 29, 0, 4, 188, 1, 0x08);
    fill(1, 1152, 64, 4, 3, 190, 1, 0x03);
    fill(2, 1166, 32, 10, 4, 191, 1, 0x28);
    // 伪造 MIXTUREBASE：14 条 3B 材料，填可辨识字节 0xA0+idx。
    uint8_t orig_mixture[14 * cr::kMixtureRecordSize];
    for (size_t i = 0; i < 14; ++i) {
        std::memset(orig_mixture + i * cr::kMixtureRecordSize, static_cast<int>(0xA0 + i),
                    cr::kMixtureRecordSize);
    }

    CHECK(cr::derive_material_count(orig_recipe, 3, cr::kRecipeRecordSize) == 14);

    size_t n = 0;
    const cr::Def* cat = cr::catalog(&n);  // N=2：{28,3} 与 {15,1}，均 group 3。
    CHECK(n == 2);

    uint8_t out_recipe[5 * cr::kRecipeRecordSize];
    uint8_t out_mixture[(14 + 2) * cr::kMixtureRecordSize];
    std::memset(out_recipe, 0, sizeof(out_recipe));
    std::memset(out_mixture, 0, sizeof(out_mixture));

    const uint32_t result =
        cr::inject_into_buffers(orig_recipe, 3, cr::kRecipeRecordSize, orig_mixture,
                                cr::kMixtureRecordSize, cat, n, out_recipe, out_mixture);
    CHECK(result == 5);  // base 3 + N 2（真机 = 69 + 2 = 71）

    // 原版记录：b0..b10 逐字节不变（12..15 的字段仍是 gemcraft 合成与费用读取的依据），
    // b11 仅被清掉模块占用的组位（0x08）。
    for (size_t r = 0; r < 3; ++r) {
        const uint8_t* src = orig_recipe + r * cr::kRecipeRecordSize;
        const uint8_t* dst = out_recipe + r * cr::kRecipeRecordSize;
        CHECK(std::memcmp(src, dst, cr::kRbGroup) == 0);
    }
    CHECK(out_recipe[0 * cr::kRecipeRecordSize + cr::kRbGroup] == 0x00);  // 0x08 → 0x00
    CHECK(out_recipe[1 * cr::kRecipeRecordSize + cr::kRbGroup] == 0x03);  // 不含 bit3 → 原样
    CHECK(out_recipe[2 * cr::kRecipeRecordSize + cr::kRbGroup] == 0x20);  // 0x28 → 0x20（bit5 保留）

    // MIXTUREBASE 前段原样复制（14 条 = 42B）。
    CHECK(std::memcmp(out_mixture, orig_mixture, sizeof(orig_mixture)) == 0);

    // 注入记录 1（下标 base → 「合成」）：镜像 record 12 的材料/费用，b11=0x28。
    const uint8_t* inj0 = out_recipe + 3 * cr::kRecipeRecordSize;
    CHECK(rd_u16(inj0 + cr::kRbLabel) == 35216);
    CHECK(rd_u16(inj0 + cr::kRbResultId) == 0);
    CHECK(rd_u16(inj0 + cr::kRbMaterialStart) == 14);  // base_material_count
    CHECK(inj0[cr::kRbMaterialCount] == 1);
    CHECK(inj0[cr::kRbFlag7] == 1);
    CHECK(rd_u16(inj0 + cr::kRbCostWord) == 188);
    CHECK(inj0[cr::kRbUnlockGate] == 0);                // 必须 0（§7.8）
    CHECK(inj0[cr::kRbGroup] == 0x28);                 // bit3=group3（宝石强化页）+ bit5=配方书位

    // 注入记录 2（下标 base+1 → 「宝石强化」）：费用公式必须合法（188，同 record 12），b11=0x28。
    const uint8_t* inj1 = out_recipe + 4 * cr::kRecipeRecordSize;
    CHECK(rd_u16(inj1 + cr::kRbLabel) == 35291);
    CHECK(rd_u16(inj1 + cr::kRbMaterialStart) == 15);  // 14 + 1
    CHECK(inj1[cr::kRbMaterialCount] == 1);
    CHECK(rd_u16(inj1 + cr::kRbCostWord) == 188);
    CHECK(inj1[cr::kRbUnlockGate] == 0);
    CHECK(inj1[cr::kRbGroup] == 0x28);

    // 追加材料条目：MIXTUREBASE[14] = {28,3}（合成），[15] = {15,1}（宝石强化）。
    const uint8_t* mat0 = out_mixture + 14 * cr::kMixtureRecordSize;
    CHECK(rd_u16(mat0) == 28);
    CHECK(mat0[2] == 3);
    const uint8_t* mat1 = out_mixture + 15 * cr::kMixtureRecordSize;
    CHECK(rd_u16(mat1) == 15);
    CHECK(mat1[2] == 1);

    // 幂等：相同输入重复调用产生完全相同的输出。
    uint8_t out_recipe2[5 * cr::kRecipeRecordSize];
    uint8_t out_mixture2[(14 + 2) * cr::kMixtureRecordSize];
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

// ---------------------------------------------------------------------------
// 模块配方描述文案常量：hook 门控与刷新的字节预算前提。
// kModuleRecipeDesc 必须 = 36 字节 UTF-8 + NUL = 37，且 ≤ 刷新 max_len 0x166(358)；
// 结果占位 itemId 必须 = 0（注入记录 b2-3，门控据此识别「配方描述」调用）。
// ---------------------------------------------------------------------------
static void test_module_recipe_desc_constants() {
    CHECK(sizeof(cr::kModuleRecipeDesc) == 20);
    CHECK(std::strlen(cr::kModuleRecipeDesc) == 19);
    CHECK(std::strcmp(cr::kModuleRecipeDesc, "用3个材料合成") == 0);
    CHECK(cr::kModuleRecipeResultItemId == 0);
}

// ---------------------------------------------------------------------------
// §2.8 3 格配方：可堆叠物品的放料上限与合成前库存复核（纯逻辑）
// 语义：每格 = 1 件；可堆叠物品 = 1 个单位，同一个堆可以占多格，但受**类别持有总数**约束。
// ---------------------------------------------------------------------------
static void test_three_slot_stack_rules() {
    // 放料上限：该类别已占格数必须**严格小于**类别持有总数（「2 个物品不能添加 3 次」）。
    CHECK(cr::slot_add_allowed(0, 2));
    CHECK(cr::slot_add_allowed(1, 2));
    CHECK(!cr::slot_add_allowed(2, 2));  // 已占 2 格、堆内 2 个 → 第 3 次拒绝
    CHECK(cr::slot_add_allowed(1, 5));
    CHECK(!cr::slot_add_allowed(5, 5));
    // 非法输入 fail-closed。
    CHECK(!cr::slot_add_allowed(-1, 5));
    CHECK(!cr::slot_add_allowed(0, 0));
    CHECK(!cr::slot_add_allowed(0, -3));

    // 合成前复核：需扣单位数 ≤ 类别持有总数；无需扣减（units<=0）恒通过。
    CHECK(cr::stack_units_available(2, 2));
    CHECK(!cr::stack_units_available(3, 2));  // 库存不足 → 调用方必须中止且不消耗
    CHECK(cr::stack_units_available(1, 1));
    CHECK(!cr::stack_units_available(1, 0));
    CHECK(cr::stack_units_available(0, 0));
    CHECK(cr::stack_units_available(-1, 0));
}

// ---------------------------------------------------------------------------
// §2.8 混沌卷轴配方：宝石 + 混沌武器强化卷轴(20) + 混沌防具强化卷轴(25)
//                     → 该宝石自身（同类别），数值 ×1.2 向上取整
// 第 1 格为通配宝石槽（任意 28..32），且**槽位严格顺序**。
// ---------------------------------------------------------------------------
static void test_chaos_scroll_recipe() {
    size_t n = 0;
    const cr::ThreeSlotRecipe* recipes = cr::three_slot_recipes(&n);
    CHECK(recipes != nullptr);
    CHECK(n == 8);  // 6 条既有 + 混沌卷轴 + 特殊装备拉满

    // 任意宝石类别都能填第 1 格（通配槽 28..32）。
    for (uint16_t gem = 28; gem <= 32; ++gem) {
        const uint16_t slots[3] = {gem, 20, 25};
        const cr::ThreeSlotRecipe* hit = cr::match_three_slot(slots);
        CHECK(hit == &recipes[6]);
        if (hit == &recipes[6]) {
            CHECK(hit->ordered);
            CHECK(hit->product_mode == cr::ProductMode::kScaleFirstItem);
            CHECK(hit->scale_permille == 1200);
        }
    }
    // 顺序严格：换位 / 缺格 / 两格同料 一律不命中（也不得落到其它配方）。
    const uint16_t wrong_order[3] = {20, 28, 25};
    CHECK(cr::match_three_slot(wrong_order) == nullptr);
    const uint16_t wrong_order2[3] = {28, 25, 20};
    CHECK(cr::match_three_slot(wrong_order2) == nullptr);
    const uint16_t missing_armor[3] = {28, 20, 0};
    CHECK(cr::match_three_slot(missing_armor) == nullptr);
    const uint16_t two_scrolls[3] = {20, 20, 25};
    CHECK(cr::match_three_slot(two_scrolls) == nullptr);
    // 第 1 格必须是宝石：塞卷轴不命中。
    const uint16_t non_jewel[3] = {16, 20, 25};
    CHECK(cr::match_three_slot(non_jewel) == nullptr);

    // 既有 6 条配方保持「固定类别产物」语义（本次扩展不得改动它们）。
    for (size_t i = 0; i < 6; ++i) {
        CHECK(recipes[i].product_mode == cr::ProductMode::kFixedCategory);
    }
    const uint16_t leather[3] = {35, 0, 41};
    const cr::ThreeSlotRecipe* leather_hit = cr::match_three_slot(leather);
    CHECK(leather_hit != nullptr && leather_hit->product == 4);
    const uint16_t three_gems[3] = {28, 28, 28};
    const cr::ThreeSlotRecipe* gem_hit = cr::match_three_slot(three_gems);
    CHECK(gem_hit != nullptr && gem_hit->product == 29);
}

// ---------------------------------------------------------------------------
// 「两件相同特殊装备 + 元气恢复药水」配方：槽位严格顺序 + 槽0/槽2 必须同类别 + 通配特殊装备槽。
// ---------------------------------------------------------------------------
static bool fake_is_special(uint16_t category) {
    return category == 485 || category == 493 || category == 948;
}

static void test_max_socket_enchant_recipe() {
    size_t n = 0;
    const cr::ThreeSlotRecipe* recipes = cr::three_slot_recipes(&n);
    CHECK(recipes != nullptr);
    CHECK(n == 8);
    const cr::ThreeSlotRecipe* r = &recipes[7];
    CHECK(r->ordered);
    CHECK(r->same_first_last);
    CHECK(r->product_mode == cr::ProductMode::kMaxSocketEnchantFirstItem);
    CHECK(r->slots[0] == cr::kAnySpecialEquipSlot);
    CHECK(r->slots[1] == 14);  // 元气恢复药水
    CHECK(r->slots[2] == cr::kAnySpecialEquipSlot);

    // 命中：两件同类别特殊装备 + 药水，顺序 = 装备 / 药水 / 装备。
    const uint16_t ok[3] = {485, 14, 485};
    CHECK(cr::match_three_slot(ok, &fake_is_special) == r);
    const uint16_t ok2[3] = {948, 14, 948};
    CHECK(cr::match_three_slot(ok2, &fake_is_special) == r);

    // 顺序严格：药水挪到 1/3 位都不命中。
    const uint16_t wrong1[3] = {14, 485, 485};
    CHECK(cr::match_three_slot(wrong1, &fake_is_special) == nullptr);
    const uint16_t wrong2[3] = {485, 485, 14};
    CHECK(cr::match_three_slot(wrong2, &fake_is_special) == nullptr);

    // 两件必须同类：混不同特殊装备不命中。
    const uint16_t mixed[3] = {485, 14, 493};
    CHECK(cr::match_three_slot(mixed, &fake_is_special) == nullptr);

    // 非特殊装备不命中；谓词缺省时该配方同样不命中（fail-closed）。
    const uint16_t plain[3] = {334, 14, 334};
    CHECK(cr::match_three_slot(plain, &fake_is_special) == nullptr);
    CHECK(cr::match_three_slot(ok) == nullptr);

    // 谓词缺省不得影响其它配方。
    const uint16_t three_gems[3] = {28, 28, 28};
    CHECK(cr::match_three_slot(three_gems) != nullptr);
}

// ---------------------------------------------------------------------------
// 动态特殊装备配方：进档随机生成「装备 → 3 个不同材料（顺序严格）」的映射。
// ---------------------------------------------------------------------------
static const int* g_rand_seq = nullptr;
static size_t g_rand_len = 0;
static size_t g_rand_pos = 0;

// 可编程随机序列：取值 = lo + (seq[i] mod span)。测试用它同时驱动「是否为空」与「选哪个材料」。
static void set_rand_seq(const int* seq, size_t len) {
    g_rand_seq = seq;
    g_rand_len = len;
    g_rand_pos = 0;
}

static int fake_rand(int lo, int hi) {
    const int span = hi - lo + 1;
    if (span <= 0 || g_rand_seq == nullptr || g_rand_len == 0) return lo;
    const int v = g_rand_seq[g_rand_pos++ % g_rand_len];
    return lo + (v % span);
}

static void test_dynamic_special_recipes() {
    cr::set_dynamic_three_slot_recipes(nullptr, 0);  // 前置：从空表开始
    const uint16_t equips[2] = {485, 493};
    const uint16_t pool[4] = {33, 35, 41, 57};  // 秘银/皮革/魔法衣料/生命之叶
    // 池下标 4（== pool_size）是「空」项 → 类别 0。
    constexpr size_t kEmptyIdx = 4;
    cr::ThreeSlotRecipe out[8];

    // A) 三格全有料：抽到池下标 0/1/2 → 33/35/41。顺序 = 抽取次序。
    {
        const int seq[] = {0, 1, 2};
        set_rand_seq(seq, 3);
        const size_t n = cr::build_dynamic_recipes(equips, 1, pool, 4, &fake_rand, out, 8);
        CHECK(n == 1);
        CHECK(out[0].ordered);
        CHECK(out[0].product == 485);
        CHECK(out[0].product_mode == cr::ProductMode::kFixedCategory);
        CHECK(out[0].slots[0] == 33);
        CHECK(out[0].slots[1] == 35);
        CHECK(out[0].slots[2] == 41);
    }

    // B) 「空」是池里的普通候选：抽到空项 → 该格写 0（匹配语义 = 该格必须为空）。
    {
        const int seq[] = {0, 1, static_cast<int>(kEmptyIdx)};
        set_rand_seq(seq, 3);
        const size_t n = cr::build_dynamic_recipes(equips, 1, pool, 4, &fake_rand, out, 8);
        CHECK(n == 1);
        CHECK(out[0].slots[0] == 33);
        CHECK(out[0].slots[1] == 35);
        CHECK(out[0].slots[2] == 0);
    }

    // C) 空可以落在任意格（含第 0 格）。
    {
        const int seq[] = {static_cast<int>(kEmptyIdx), 1, 2};
        set_rand_seq(seq, 3);
        const size_t n = cr::build_dynamic_recipes(equips, 1, pool, 4, &fake_rand, out, 8);
        CHECK(n == 1);
        CHECK(out[0].slots[0] == 0);
        CHECK(out[0].slots[1] == 35);
        CHECK(out[0].slots[2] == 41);
    }

    // D) 「3 个不同」按池下标去重 ⇒ 空项最多出现一次（不可能三格全空）。
    {
        const int seq[] = {static_cast<int>(kEmptyIdx), static_cast<int>(kEmptyIdx), 2, 3};
        set_rand_seq(seq, 4);
        const size_t n = cr::build_dynamic_recipes(equips, 1, pool, 4, &fake_rand, out, 8);
        CHECK(n == 1);
        const int zeros = (out[0].slots[0] == 0) + (out[0].slots[1] == 0) + (out[0].slots[2] == 0);
        CHECK(zeros <= 1);
    }

    // E) 两条装备各生成一条；空格语义经 match_three_slot 生效（空格要求该格确实为空）。
    {
        const int seq[] = {0, 1, 2, static_cast<int>(kEmptyIdx), 3, 0};
        set_rand_seq(seq, 6);
        const size_t n = cr::build_dynamic_recipes(equips, 2, pool, 4, &fake_rand, out, 8);
        CHECK(n == 2);
        CHECK(out[0].product == 485);
        CHECK(out[1].product == 493);
        cr::set_dynamic_three_slot_recipes(out, n);
        CHECK(cr::dynamic_three_slot_recipe_count() == 2);
        const uint16_t hit0[3] = {out[0].slots[0], out[0].slots[1], out[0].slots[2]};
        const cr::ThreeSlotRecipe* h0 = cr::match_three_slot(hit0);
        CHECK(h0 != nullptr && h0->product == 485);
        // 顺序严格：换位后不得再命中 485。
        const uint16_t swap0[3] = {out[0].slots[1], out[0].slots[0], out[0].slots[2]};
        const cr::ThreeSlotRecipe* s0 = cr::match_three_slot(swap0);
        CHECK(s0 == nullptr || s0->product != 485);
        // 第 2 条含空格：把该格填上料就不该命中。
        if (out[1].slots[0] == 0) {
            const uint16_t filled[3] = {33, out[1].slots[1], out[1].slots[2]};
            const cr::ThreeSlotRecipe* f = cr::match_three_slot(filled);
            CHECK(f == nullptr || f->product != 493);
        }
    }

    // FAIL-CLOSED：池不足 3 条 / 随机源为空 / 容量为 0。
    CHECK(cr::build_dynamic_recipes(equips, 2, pool, 2, &fake_rand, out, 8) == 0);
    CHECK(cr::build_dynamic_recipes(equips, 2, pool, 4, nullptr, out, 8) == 0);
    CHECK(cr::build_dynamic_recipes(equips, 2, pool, 4, &fake_rand, out, 0) == 0);

    // 清空后动态配方立即失效。
    cr::set_dynamic_three_slot_recipes(nullptr, 0);
    CHECK(cr::dynamic_three_slot_recipe_count() == 0);
}

// 宝石数值缩放：**ceil**(value × permille / 1000)，钳到 [0, 2047]。
static void test_scaled_jewel_value() {
    // ×1.2 向上取整。
    CHECK(cr::scaled_jewel_value(100, 1200) == 120);
    CHECK(cr::scaled_jewel_value(10, 1200) == 12);
    CHECK(cr::scaled_jewel_value(5, 1200) == 6);    // 恰好 6.0
    CHECK(cr::scaled_jewel_value(3, 1200) == 4);    // ceil(3.6)=4（向下取整会得 3）
    CHECK(cr::scaled_jewel_value(101, 1200) == 122);  // ceil(121.2)
    CHECK(cr::scaled_jewel_value(1, 1200) == 2);      // ceil(1.2)，不会变成 0
    // 1000 千分比 = 原值；0 = 未配置 → fail-closed 返回原值（绝不清零）。
    CHECK(cr::scaled_jewel_value(500, 1000) == 500);
    CHECK(cr::scaled_jewel_value(500, 0) == 500);
    // 上限钳制（bits0-10 = 2047）。
    CHECK(cr::scaled_jewel_value(2047, 1200) == 2047);
    CHECK(cr::scaled_jewel_value(2000, 1200) == 2047);
    CHECK(cr::scaled_jewel_value(2047, 1000) == 2047);
    // 非正输入。
    CHECK(cr::scaled_jewel_value(0, 1200) == 0);
    CHECK(cr::scaled_jewel_value(-5, 1200) == 0);
}

// ---------------------------------------------------------------------------
// §5.1(6) 配方书位（b11 bit5）不变式：
// - 每条注入记录都必须带 bit5。注入 N 条 → GetRecipeCount(5) 增加 N →
//   `base = 总记录数 − GetRecipeCount(5)` 保持不变（原版 69−52=17）。
//   否则传说装备页的「位 ↔ 记录」映射整体偏移 N 条，且 idx < base 的记录解算出**负位索引**，
//   而 AddRecipeBook 只校验上界 → 对书名册缓冲前方越界读改写。
// - 书名册字节数 (count5+7)/8 在注入前后必须相等（缓冲按注入前分配、存档位图长度按它动态）。
// ---------------------------------------------------------------------------
static void test_recipe_book_bit_invariant() {
    uint8_t recs[3 * cr::kRecipeRecordSize];
    std::memset(recs, 0, sizeof(recs));
    recs[0 * cr::kRecipeRecordSize + cr::kRbGroup] = 0x20;  // 仅配方书位
    recs[1 * cr::kRecipeRecordSize + cr::kRbGroup] = 0x28;  // 配方书 + group3
    recs[2 * cr::kRecipeRecordSize + cr::kRbGroup] = 0x08;  // 仅 group3（不计入）
    CHECK(cr::count_recipe_book_records(recs, 3, cr::kRecipeRecordSize) == 2);
    CHECK(cr::count_recipe_book_records(recs, 0, cr::kRecipeRecordSize) == 0);
    CHECK(cr::count_recipe_book_records(nullptr, 3, cr::kRecipeRecordSize) == 0);
    CHECK(cr::count_recipe_book_records(recs, 3, cr::kRbGroup) == 0);  // record_size 过小 → 0

    // 原版实测：69 条记录、其中 52 条 bit5 → 书名册 7 字节；注入 2 条后 54 → 仍 7 字节（不变式成立）。
    CHECK(cr::recipe_book_bytes(52) == 7);
    CHECK(cr::recipe_book_bytes(54) == 7);
    // 边界：+4 仍 7 字节，+5（=57）变 8 字节 → 该情形必须被 ensure() 拒绝注入。
    CHECK(cr::recipe_book_bytes(52 + 4) == 7);
    CHECK(cr::recipe_book_bytes(52 + 5) == 8);

    // 目录里每条注入记录的 b11 都必须同时带组位与配方书位。
    size_t n = 0;
    const cr::Def* cat = cr::catalog(&n);
    CHECK(n > 0);
    for (size_t i = 0; i < n; ++i) {
        uint8_t out[cr::kRecipeRecordSize];
        cr::build_record_bytes(cat[i], 0, out);
        CHECK(out[cr::kRbGroup] ==
              static_cast<uint8_t>(cr::recipe_group_bit(cat[i].group) | cr::kRbRecipeBookBit));
        CHECK((out[cr::kRbGroup] & cr::kRbRecipeBookBit) != 0);
    }
}

int main() {
    test_interval_inverse_boundaries();
    test_random_matches_target();
    test_catalog_mapping();
    test_inject_bytes();
    test_material_count();
    test_module_recipe_desc_constants();
    test_three_slot_stack_rules();
    test_chaos_scroll_recipe();
    test_max_socket_enchant_recipe();
    test_dynamic_special_recipes();
    test_scaled_jewel_value();
    test_recipe_book_bit_invariant();
    std::printf("custom_recipe_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
