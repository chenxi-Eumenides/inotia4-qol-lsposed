#include "feature/autosell/autosell_rules.h"

#include <cstdint>
#include <cstdio>

// host 单测：autosell 纯规则引擎 should_sell 的命中/不命中、边界、
// 装备三规则同类 AND（0=关不参与）、宝石同类 OR、跨类 OR、
// 值即开关（0=关闭 / 1-based 档位 / 越界物品值不命中）、总开关、
// 特殊多选与「无规则不出售」；硬保护（已强化/已镶嵌永不出售，用户 2026-09-15）。
// 强化口径 = 剩余强化次数 enhance_remaining（I_ENCHANT bits2-5，UI 名称「强化耐久度」）。
// 风格与 tests/test_attribute_range.cpp 一致（自带断言计数）。

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using autosell::Config;
using autosell::ItemView;
using autosell::should_sell;

namespace {

// 全部维度取最大档（含最低档到上限），用于总开关/兜底语义测试。
Config all_rules_on() {
    Config cfg;
    cfg.enabled = true;
    cfg.rarity = 5;       // 阈值 4（紫），rarity 0..4 全命中
    cfg.enhance = 32;     // 阈值 31 >= 字段上限 15，enhance_remaining 0..15 全命中
    cfg.socket = 16;      // 阈值 15，socket_total 0..15 命中
    cfg.gem_tier = 5;     // 阈值 4（混沌），tier 0..4 全命中
    cfg.gem_range = 5;    // 阈值 99：百分位 0..99 命中，100（满分）不命中
    cfg.special_mask = autosell::kSpecialBackpack | autosell::kSpecialNormalSeal |
                       autosell::kSpecialDice;
    return cfg;
}

ItemView equip_item() {
    ItemView item;
    item.is_equip = true;
    return item;
}

ItemView jewel_item() {
    ItemView item;
    item.is_jewel = true;
    return item;
}

}  // namespace

static void test_master_switch_off() {
    // 总开关关闭时，即使全部规则开启、物品全维度命中，也恒不出售。
    Config off = all_rules_on();
    off.enabled = false;

    ItemView equip = equip_item();
    equip.rarity = 0;
    equip.enhance_remaining = 0;
    equip.socket_total = 0;
    equip.special_types = autosell::kSpecialDice;
    CHECK(!should_sell(equip, off));

    ItemView jewel = jewel_item();
    jewel.jewel_tier = 0;
    jewel.special_types = autosell::kSpecialBackpack;
    CHECK(!should_sell(jewel, off));
}

static void test_rarity_value_switch() {
    Config cfg;
    cfg.enabled = true;
    ItemView item = equip_item();

    // 值 0 = 关闭：最低品质也不命中。
    cfg.rarity = 0;
    item.rarity = 0;
    CHECK(!should_sell(item, cfg));

    // 值 1 = 含最低档（卖 rarity <= 0）；rarity 1 不命中。
    cfg.rarity = 1;
    item.rarity = 0;
    CHECK(should_sell(item, cfg));
    item.rarity = 1;
    CHECK(!should_sell(item, cfg));

    // 中间档：值 3 → 阈值 2；rarity 2 命中、3 不命中。
    cfg.rarity = 3;
    item.rarity = 2;
    CHECK(should_sell(item, cfg));
    item.rarity = 3;
    CHECK(!should_sell(item, cfg));

    // 值 5 = 含上限（卖 rarity <= 4）：0..4 全命中。
    cfg.rarity = 5;
    for (int r = 0; r <= 4; ++r) {
        item.rarity = r;
        CHECK(should_sell(item, cfg));
    }
    // 越界物品值（rarity 5）不命中最高档。
    item.rarity = 5;
    CHECK(!should_sell(item, cfg));
}

static void test_enhance_value_switch() {
    Config cfg;
    cfg.enabled = true;
    ItemView item = equip_item();

    // 值 0 = 关闭。
    cfg.enhance = 0;
    item.enhance_remaining = 0;
    CHECK(!should_sell(item, cfg));

    // 值 1 = 含最低档（卖剩余 0 次）；剩余 1 不命中。
    cfg.enhance = 1;
    item.enhance_remaining = 0;
    CHECK(should_sell(item, cfg));
    item.enhance_remaining = 1;
    CHECK(!should_sell(item, cfg));

    // 中间档：值 8 → 阈值 7。
    cfg.enhance = 8;
    item.enhance_remaining = 7;
    CHECK(should_sell(item, cfg));
    item.enhance_remaining = 8;
    CHECK(!should_sell(item, cfg));

    // 值 16 = 含字段上限（bits2-5 最大 15，卖 <= 15）：剩余 15 命中。
    cfg.enhance = 16;
    item.enhance_remaining = 15;
    CHECK(should_sell(item, cfg));

    // 配置上限 32（阈值 31 > 字段上限）：字段全域 0..15 均命中。
    cfg.enhance = 32;
    for (int r = 0; r <= 15; ++r) {
        item.enhance_remaining = r;
        CHECK(should_sell(item, cfg));
    }
}

static void test_socket_value_switch() {
    Config cfg;
    cfg.enabled = true;
    ItemView item = equip_item();

    // 值 0 = 关闭。
    cfg.socket = 0;
    item.socket_total = 0;
    CHECK(!should_sell(item, cfg));

    // 值 1 = 含最低档（卖 <= 0 孔）；1 孔不命中。
    cfg.socket = 1;
    item.socket_total = 0;
    CHECK(should_sell(item, cfg));
    item.socket_total = 1;
    CHECK(!should_sell(item, cfg));

    // 中间档：值 6 → 阈值 5。
    cfg.socket = 6;
    item.socket_total = 5;
    CHECK(should_sell(item, cfg));
    item.socket_total = 6;
    CHECK(!should_sell(item, cfg));

    // 值 16 = 含上限（卖 <= 15）：15 命中、越界 16 不命中。
    cfg.socket = 16;
    item.socket_total = 15;
    CHECK(should_sell(item, cfg));
    item.socket_total = 16;
    CHECK(!should_sell(item, cfg));
}

static void test_hard_protection_enhanced_or_filled() {
    // 硬保护（用户 2026-09-15 裁决）：已强化（I_ENCHANT bits6-10>0）或已镶嵌
    // （I_SOCKET bits0-3>0）永不出售——即便全部规则开启且其他维度命中。
    Config cfg = all_rules_on();

    // 已强化 1 次：各维度均命中也不卖。
    ItemView enhanced = equip_item();
    enhanced.rarity = 0;
    enhanced.enhance_remaining = 0;
    enhanced.socket_total = 0;
    enhanced.enhance_level = 1;
    CHECK(!should_sell(enhanced, cfg));

    // 已镶嵌 1 孔：不卖。
    ItemView filled = equip_item();
    filled.socket_filled = 1;
    CHECK(!should_sell(filled, cfg));

    // 两者兼有：不卖。
    enhanced.socket_filled = 2;
    CHECK(!should_sell(enhanced, cfg));

    // 硬保护无条件生效：特殊类型命中（骰子）也被拦下。
    ItemView special;
    special.special_types = autosell::kSpecialDice;
    special.enhance_level = 2;
    CHECK(!should_sell(special, cfg));
    special.enhance_level = 0;
    special.socket_filled = 3;
    CHECK(!should_sell(special, cfg));

    // 对照组：两字段为 0 时不触发硬保护，规则照常命中出售。
    ItemView plain = equip_item();
    CHECK(should_sell(plain, cfg));
}

static void test_gem_tier_value_switch() {
    Config cfg;
    cfg.enabled = true;
    ItemView jewel = jewel_item();

    // 值 0 = 关闭。
    cfg.gem_tier = 0;
    jewel.jewel_tier = 0;
    CHECK(!should_sell(jewel, cfg));

    // 值 1 = 含最低档（卖 tier <= 0）；tier 1 不命中。
    cfg.gem_tier = 1;
    jewel.jewel_tier = 0;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_tier = 1;
    CHECK(!should_sell(jewel, cfg));

    // 中间档：值 3 → 阈值 2。
    cfg.gem_tier = 3;
    jewel.jewel_tier = 2;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_tier = 3;
    CHECK(!should_sell(jewel, cfg));

    // 值 5 = 含上限（卖 tier <= 4）：0..4 全命中；越界 5 不命中。
    cfg.gem_tier = 5;
    for (int t = 0; t <= 4; ++t) {
        jewel.jewel_tier = t;
        CHECK(should_sell(jewel, cfg));
    }
    jewel.jewel_tier = 5;
    CHECK(!should_sell(jewel, cfg));
}

static void test_gem_range_value_switch() {
    // 宝石属性范围：出售百分位 <= 阈值 的低品质宝石（0=关；1..5 -> 30/60/75/90/99）。
    Config cfg;
    cfg.enabled = true;
    ItemView jewel = jewel_item();

    // 值 0 = 关闭：最低百分位也不命中。
    cfg.gem_range = 0;
    jewel.jewel_percentile = 0;
    CHECK(!should_sell(jewel, cfg));

    // 值 1 → 阈值 30：0..30 命中，31 不命中。
    cfg.gem_range = 1;
    jewel.jewel_percentile = 0;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_percentile = 30;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_percentile = 31;
    CHECK(!should_sell(jewel, cfg));

    // 值 2 → 60；值 3 → 75；值 4 → 90：边界含阈值，越界不命中。
    cfg.gem_range = 2;
    jewel.jewel_percentile = 60;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_percentile = 61;
    CHECK(!should_sell(jewel, cfg));

    cfg.gem_range = 3;
    jewel.jewel_percentile = 75;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_percentile = 76;
    CHECK(!should_sell(jewel, cfg));

    cfg.gem_range = 4;
    jewel.jewel_percentile = 90;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_percentile = 91;
    CHECK(!should_sell(jewel, cfg));

    // 值 5 → 阈值 99：百分位 0..99 命中，100（满分）不命中。
    cfg.gem_range = 5;
    for (int p = 0; p <= 90; p += 10) {
        jewel.jewel_percentile = p;
        CHECK(should_sell(jewel, cfg));
    }
    jewel.jewel_percentile = 99;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_percentile = 100;
    CHECK(!should_sell(jewel, cfg));

    // 未知百分位（-1，探测失败/未安装）：任何档位都不命中（fail-closed）。
    for (int g = 0; g <= 5; ++g) {
        cfg.gem_range = g;
        jewel.jewel_percentile = -1;
        CHECK(!should_sell(jewel, cfg));
    }

    // 越界档位（<0 / >5）视为关闭，不命中。
    jewel.jewel_percentile = 0;
    cfg.gem_range = -1;
    CHECK(!should_sell(jewel, cfg));
    cfg.gem_range = 6;
    CHECK(!should_sell(jewel, cfg));
}

static void test_gem_range_only_jewels_and_or() {
    // 仅宝石生效：装备/普通物品即使 jewel_percentile=0 也不被 gemRange 命中。
    Config cfg;
    cfg.enabled = true;
    cfg.gem_range = 5;

    ItemView equip = equip_item();
    equip.jewel_percentile = 0;
    CHECK(!should_sell(equip, cfg));

    ItemView plain;
    plain.jewel_percentile = 0;
    CHECK(!should_sell(plain, cfg));

    // 与 gemTier 同类内 OR：gemTier 关闭但 gemRange 命中 -> 出售。
    ItemView jewel = jewel_item();
    Config range_only;
    range_only.enabled = true;
    range_only.gem_tier = 0;
    range_only.gem_range = 1;
    jewel.jewel_tier = 4;  // gemTier 关闭，忽略
    jewel.jewel_percentile = 10;
    CHECK(should_sell(jewel, range_only));

    // gemRange 关闭但 gemTier 命中 -> 仍出售（互不影响）。
    Config tier_only;
    tier_only.enabled = true;
    tier_only.gem_tier = 5;
    tier_only.gem_range = 0;
    jewel.jewel_tier = 0;
    jewel.jewel_percentile = -1;
    CHECK(should_sell(jewel, tier_only));

    // 总开关关闭：gemRange 命中也不出售。
    Config off = range_only;
    off.enabled = false;
    jewel.jewel_percentile = 10;
    CHECK(!should_sell(jewel, off));
}

static void test_equip_rules_and() {
    // 装备三规则同类 AND（用户 2026-09-16）：0=关不参与；已开启项须同时命中才出售。
    // 物品：rarity 2 / enhance_remaining 3 / socket_total 2。
    // 命中档位：rarity 值 3→阈值 2、enhance 值 4→阈值 3、socket 值 3→阈值 2。
    ItemView item = equip_item();
    item.rarity = 2;
    item.enhance_remaining = 3;
    item.socket_total = 2;

    // 三条全开且全部命中 -> 出售。
    Config all_hit;
    all_hit.enabled = true;
    all_hit.rarity = 3;
    all_hit.enhance = 4;
    all_hit.socket = 3;
    CHECK(should_sell(item, all_hit));

    // 单条开启：单条 AND 退化为「该条命中即出售」。
    Config only_rarity;
    only_rarity.enabled = true;
    only_rarity.rarity = 3;
    CHECK(should_sell(item, only_rarity));

    Config only_enhance;
    only_enhance.enabled = true;
    only_enhance.enhance = 4;
    CHECK(should_sell(item, only_enhance));

    Config only_socket;
    only_socket.enabled = true;
    only_socket.socket = 3;
    CHECK(should_sell(item, only_socket));

    // 单条开启但不命中 -> 不出售。
    Config only_rarity_miss;
    only_rarity_miss.enabled = true;
    only_rarity_miss.rarity = 2;  // 阈值 1，rarity 2 不命中
    CHECK(!should_sell(item, only_rarity_miss));

    // 三条全开但中间一条（强化）不命中 -> 不出售（AND）。
    Config enhance_miss;
    enhance_miss.enabled = true;
    enhance_miss.rarity = 3;   // 阈值 2，命中
    enhance_miss.enhance = 3;  // 阈值 2，enhance 3 不命中
    enhance_miss.socket = 3;   // 阈值 2，命中
    CHECK(!should_sell(item, enhance_miss));

    // 三条全开但首条（品质）不命中 -> 不出售（AND）。
    Config rarity_miss;
    rarity_miss.enabled = true;
    rarity_miss.rarity = 2;   // 阈值 1，rarity 2 不命中
    rarity_miss.enhance = 4;  // 阈值 3，命中
    rarity_miss.socket = 3;   // 阈值 2，命中
    CHECK(!should_sell(item, rarity_miss));

    // 三条全开但末条（孔位）不命中 -> 不出售（AND）。
    Config socket_miss;
    socket_miss.enabled = true;
    socket_miss.rarity = 3;   // 阈值 2，命中
    socket_miss.enhance = 4;  // 阈值 3，命中
    socket_miss.socket = 2;   // 阈值 1，socket 2 不命中
    CHECK(!should_sell(item, socket_miss));

    // 关闭项不参与：两条开启且均命中、第三条关闭 -> 出售。
    Config socket_off;
    socket_off.enabled = true;
    socket_off.rarity = 3;
    socket_off.enhance = 4;
    socket_off.socket = 0;  // 关闭
    CHECK(should_sell(item, socket_off));

    Config rarity_off;
    rarity_off.enabled = true;
    rarity_off.rarity = 0;  // 关闭
    rarity_off.enhance = 4;
    rarity_off.socket = 3;
    CHECK(should_sell(item, rarity_off));

    // 三条全部关闭 -> 不参与，不出售。
    Config none_enabled;
    none_enabled.enabled = true;
    none_enabled.rarity = 0;
    none_enabled.enhance = 0;
    none_enabled.socket = 0;
    CHECK(!should_sell(item, none_enabled));
}

static void test_disable_dimension() {
    // 值 0 = 关闭某条规则后，该维度即使原本命中也不得出售。
    ItemView item = equip_item();
    item.rarity = 0;       // 品质维度会命中
    item.enhance_remaining = 5;
    item.socket_total = 3;

    Config cfg;
    cfg.enabled = true;
    // 品质关闭，强化/孔位开启但均不命中 -> 不出售。
    cfg.rarity = 0;
    cfg.enhance = 1;  // 阈值 0，enhance 5 不命中
    cfg.socket = 1;   // 阈值 0，socket 3 不命中
    CHECK(!should_sell(item, cfg));

    // 仅开启品质（值 1 -> 阈值 0）-> 命中出售，证明差异确由值开关造成。
    Config only_rarity;
    only_rarity.enabled = true;
    only_rarity.rarity = 1;
    CHECK(should_sell(item, only_rarity));

    // 宝石规则关闭（值 0）时不因档位命中。
    ItemView jewel = jewel_item();
    jewel.jewel_tier = 0;
    Config gem_off;
    gem_off.enabled = true;
    gem_off.gem_tier = 0;
    CHECK(!should_sell(jewel, gem_off));

    // 特殊规则关闭（mask=0）时不因已勾选类型命中。
    ItemView t = equip_item();
    t.special_types = autosell::kSpecialDice;
    Config special_off;
    special_off.enabled = true;
    special_off.special_mask = 0;
    CHECK(!should_sell(t, special_off));
}

static void test_cross_category_independence() {
    Config cfg;
    cfg.enabled = true;
    cfg.rarity = 5;
    cfg.enhance = 32;
    cfg.socket = 16;
    cfg.gem_tier = 5;

    // 非装备：即使数值落在装备规则命中区间，也不被装备规则命中。
    ItemView plain;
    plain.is_equip = false;
    plain.is_jewel = false;
    plain.rarity = 0;
    plain.enhance_remaining = 0;
    plain.socket_total = 0;
    CHECK(!should_sell(plain, cfg));

    // 宝石物品不被装备规则命中（is_equip=false）。
    ItemView jewel = jewel_item();
    jewel.rarity = 0;
    jewel.enhance_remaining = 0;
    jewel.socket_total = 0;
    Config equip_only;
    equip_only.enabled = true;
    equip_only.rarity = 5;
    equip_only.enhance = 32;
    equip_only.socket = 16;
    CHECK(!should_sell(jewel, equip_only));

    // 装备物品不被宝石规则命中（is_jewel=false）。
    ItemView equip = equip_item();
    equip.jewel_tier = 0;
    Config jewel_only;
    jewel_only.enabled = true;
    jewel_only.gem_tier = 5;
    CHECK(!should_sell(equip, jewel_only));

    // 宝石物品本身可被宝石规则命中。
    CHECK(should_sell(jewel, jewel_only));
}

static void test_special_mask() {
    CHECK(autosell::kSpecialBackpack == 1u);
    CHECK(autosell::kSpecialNormalSeal == 2u);
    CHECK(autosell::kSpecialDice == 4u);

    Config cfg;
    cfg.enabled = true;
    cfg.special_mask = autosell::kSpecialBackpack | autosell::kSpecialDice;

    // 命中勾选位之一即出售。
    ItemView item;
    item.special_types = autosell::kSpecialDice;
    CHECK(should_sell(item, cfg));
    item.special_types = autosell::kSpecialBackpack;
    CHECK(should_sell(item, cfg));
    item.special_types = autosell::kSpecialBackpack | autosell::kSpecialNormalSeal;
    CHECK(should_sell(item, cfg));

    // 未勾选位不命中。
    item.special_types = 1u << 3;
    CHECK(!should_sell(item, cfg));
    item.special_types = 1u << 4;
    CHECK(!should_sell(item, cfg));
    item.special_types = 0;
    CHECK(!should_sell(item, cfg));

    // 特殊规则对任意物品生效（装备/宝石/普通均可）。
    ItemView equip = equip_item();
    equip.special_types = autosell::kSpecialDice;
    CHECK(should_sell(equip, cfg));

    ItemView jewel = jewel_item();
    jewel.special_types = autosell::kSpecialBackpack;
    CHECK(should_sell(jewel, cfg));

    // 未勾选任何特殊类型（mask=0）时不因该规则出售。
    Config empty_mask = cfg;
    empty_mask.special_mask = 0;
    item.special_types = autosell::kSpecialDice;
    CHECK(!should_sell(item, empty_mask));
}

static void test_no_rules_enabled() {
    // 总开关开启但所有规则值为 0（全关闭）：任何物品均不出售。
    Config cfg;
    cfg.enabled = true;

    ItemView equip = equip_item();
    equip.rarity = 0;
    equip.enhance_remaining = 0;
    equip.socket_total = 0;
    equip.special_types = 0xFFFFFFFFu;
    CHECK(!should_sell(equip, cfg));

    ItemView jewel = jewel_item();
    jewel.jewel_tier = 0;
    jewel.special_types = 0xFFFFFFFFu;
    CHECK(!should_sell(jewel, cfg));

    ItemView plain;
    plain.special_types = 0xFFFFFFFFu;
    CHECK(!should_sell(plain, cfg));
}

static void test_global_or_across_categories() {
    // 跨类命中任一即出售：装备规则不命中，但特殊类型命中。
    Config cfg;
    cfg.enabled = true;
    cfg.rarity = 1;  // 阈值 0
    cfg.special_mask = autosell::kSpecialNormalSeal;

    ItemView equip = equip_item();
    equip.rarity = 4;  // 装备规则不命中（4 > 0）
    equip.special_types = autosell::kSpecialNormalSeal;
    CHECK(should_sell(equip, cfg));

    // 两者均不命中 -> 不出售。
    equip.special_types = autosell::kSpecialDice;
    CHECK(!should_sell(equip, cfg));
}

int main() {
    test_master_switch_off();
    test_rarity_value_switch();
    test_enhance_value_switch();
    test_socket_value_switch();
    test_hard_protection_enhanced_or_filled();
    test_gem_tier_value_switch();
    test_gem_range_value_switch();
    test_gem_range_only_jewels_and_or();
    test_equip_rules_and();
    test_disable_dimension();
    test_cross_category_independence();
    test_special_mask();
    test_no_rules_enabled();
    test_global_or_across_categories();
    std::printf("autosell_rules_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
