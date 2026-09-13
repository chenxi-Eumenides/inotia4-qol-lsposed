#include "feature/autosell/autosell_rules.h"

#include <cstdint>
#include <cstdio>

// host 单测：autosell 纯规则引擎 should_sell 的命中/不命中、边界、同类 OR、
// 跨类独立、关闭维度、总开关、特殊多选与「无规则不出售」。
// 风格与 tests/test_attribute_range.cpp 一致（本文件自带断言计数）。

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

Config all_rules_on() {
    Config cfg;
    cfg.enabled = true;
    cfg.rarity_enabled = true;
    cfg.rarity_threshold = 4;
    cfg.enhance_enabled = true;
    cfg.enhance_threshold = 30;
    cfg.socket_enabled = true;
    cfg.socket_threshold = 15;
    cfg.gem_tier_enabled = true;
    cfg.gem_tier_threshold = 4;
    cfg.special_enabled = true;
    cfg.special_mask = autosell::kSpecialBackpack | autosell::kSpecialMercenarySeal |
                       autosell::kSpecialEnchantScroll | autosell::kSpecialDice |
                       autosell::kSpecialSealed | autosell::kSpecialItemBox;
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
    const Config cfg = all_rules_on();
    Config off = cfg;
    off.enabled = false;

    ItemView equip = equip_item();
    equip.rarity = 0;
    equip.enhance_count = 0;
    equip.socket_total = 0;
    equip.special_types = autosell::kSpecialDice;
    CHECK(!should_sell(equip, off));

    ItemView jewel = jewel_item();
    jewel.jewel_tier = 0;
    jewel.special_types = autosell::kSpecialBackpack;
    CHECK(!should_sell(jewel, off));
}

static void test_rarity_rule_boundaries() {
    Config cfg;
    cfg.enabled = true;
    cfg.rarity_enabled = true;

    // 阈值 0：仅 rarity==0 命中。
    cfg.rarity_threshold = 0;
    ItemView item = equip_item();
    item.rarity = 0;
    CHECK(should_sell(item, cfg));
    item.rarity = 1;
    CHECK(!should_sell(item, cfg));

    // 阈值 4（最大档）：rarity<=4 全命中。
    cfg.rarity_threshold = 4;
    for (int r = 0; r <= 4; ++r) {
        item.rarity = r;
        CHECK(should_sell(item, cfg));
    }
    // 阈值边界：等于阈值命中、阈值+1 不命中。
    cfg.rarity_threshold = 2;
    item.rarity = 2;
    CHECK(should_sell(item, cfg));
    item.rarity = 3;
    CHECK(!should_sell(item, cfg));
}

static void test_enhance_rule_boundaries() {
    Config cfg;
    cfg.enabled = true;
    cfg.enhance_enabled = true;

    // 阈值 0：仅 0 次命中。
    cfg.enhance_threshold = 0;
    ItemView item = equip_item();
    item.enhance_count = 0;
    CHECK(should_sell(item, cfg));
    item.enhance_count = 1;
    CHECK(!should_sell(item, cfg));

    // 阈值 30（I_ENCHANT bits6-10 上限）：30 命中、31 不命中。
    cfg.enhance_threshold = 30;
    item.enhance_count = 0;
    CHECK(should_sell(item, cfg));
    item.enhance_count = 30;
    CHECK(should_sell(item, cfg));
    item.enhance_count = 31;
    CHECK(!should_sell(item, cfg));
}

static void test_socket_rule_boundaries() {
    Config cfg;
    cfg.enabled = true;
    cfg.socket_enabled = true;

    // 阈值 0：仅 0 孔命中。
    cfg.socket_threshold = 0;
    ItemView item = equip_item();
    item.socket_total = 0;
    CHECK(should_sell(item, cfg));
    item.socket_total = 1;
    CHECK(!should_sell(item, cfg));

    // 阈值 15（I_SOCKET bits4-7 上限）：15 命中、16 不命中。
    cfg.socket_threshold = 15;
    item.socket_total = 15;
    CHECK(should_sell(item, cfg));
    item.socket_total = 16;
    CHECK(!should_sell(item, cfg));
}

static void test_gem_tier_rule_boundaries() {
    Config cfg;
    cfg.enabled = true;
    cfg.gem_tier_enabled = true;

    // 阈值 0（低级）：仅 tier==0 命中。
    cfg.gem_tier_threshold = 0;
    ItemView jewel = jewel_item();
    jewel.jewel_tier = 0;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_tier = 1;
    CHECK(!should_sell(jewel, cfg));

    // 阈值 4（混沌）：0..4 全命中。
    cfg.gem_tier_threshold = 4;
    for (int t = 0; t <= 4; ++t) {
        jewel.jewel_tier = t;
        CHECK(should_sell(jewel, cfg));
    }
    // 阈值边界：等于阈值命中、阈值+1 不命中。
    cfg.gem_tier_threshold = 2;
    jewel.jewel_tier = 2;
    CHECK(should_sell(jewel, cfg));
    jewel.jewel_tier = 3;
    CHECK(!should_sell(jewel, cfg));
}

static void test_equip_rules_or() {
    // 同类内 OR：三条装备规则中只启用命中的一条也出售。
    Config cfg;
    cfg.enabled = true;

    ItemView item = equip_item();
    item.rarity = 2;
    item.enhance_count = 3;
    item.socket_total = 2;

    // 仅品质启用且不命中（阈值 0 < rarity 2），其余关闭 -> 不出售。
    cfg.rarity_enabled = true;
    cfg.rarity_threshold = 0;
    CHECK(!should_sell(item, cfg));

    // 品质/强化均不命中，仅孔位启用且命中 -> 出售。
    cfg.rarity_enabled = true;
    cfg.rarity_threshold = 0;
    cfg.enhance_enabled = true;
    cfg.enhance_threshold = 0;
    cfg.socket_enabled = true;
    cfg.socket_threshold = 5;
    CHECK(should_sell(item, cfg));

    // 只强化命中（阈值 3），品质/孔位不命中 -> 出售。
    Config only_enhance;
    only_enhance.enabled = true;
    only_enhance.enhance_enabled = true;
    only_enhance.enhance_threshold = 3;
    CHECK(should_sell(item, only_enhance));

    // 只品质命中（阈值 2）-> 出售。
    Config only_rarity;
    only_rarity.enabled = true;
    only_rarity.rarity_enabled = true;
    only_rarity.rarity_threshold = 2;
    CHECK(should_sell(item, only_rarity));
}

static void test_disable_dimension() {
    // 关闭某条规则后，该维度即使原本命中也不得出售。
    ItemView item = equip_item();
    item.rarity = 0;       // 品质维度会命中
    item.enhance_count = 5;
    item.socket_total = 3;

    Config cfg;
    cfg.enabled = true;
    // 品质关闭，强化/孔位开启但均不命中 -> 不出售。
    cfg.rarity_enabled = false;
    cfg.rarity_threshold = 4;
    cfg.enhance_enabled = true;
    cfg.enhance_threshold = 0;
    cfg.socket_enabled = true;
    cfg.socket_threshold = 0;
    CHECK(!should_sell(item, cfg));

    // 仅开启品质 -> 命中出售，证明差异确由开关造成。
    Config only_rarity;
    only_rarity.enabled = true;
    only_rarity.rarity_enabled = true;
    only_rarity.rarity_threshold = 4;
    CHECK(should_sell(item, only_rarity));

    // 宝石规则关闭时不因档位命中。
    ItemView jewel = jewel_item();
    jewel.jewel_tier = 0;
    Config gem_off;
    gem_off.enabled = true;
    gem_off.gem_tier_enabled = false;
    gem_off.gem_tier_threshold = 4;
    CHECK(!should_sell(jewel, gem_off));

    // 特殊规则关闭时不因已勾选类型命中。
    ItemView t = equip_item();
    t.special_types = autosell::kSpecialDice;
    Config special_off;
    special_off.enabled = true;
    special_off.special_enabled = false;
    special_off.special_mask = autosell::kSpecialDice;
    CHECK(!should_sell(t, special_off));
}

static void test_cross_category_independence() {
    Config cfg;
    cfg.enabled = true;
    cfg.rarity_enabled = true;
    cfg.rarity_threshold = 4;
    cfg.enhance_enabled = true;
    cfg.enhance_threshold = 30;
    cfg.socket_enabled = true;
    cfg.socket_threshold = 15;
    cfg.gem_tier_enabled = true;
    cfg.gem_tier_threshold = 4;

    // 非装备：即使数值落在装备规则命中区间，也不被装备规则命中。
    ItemView plain;
    plain.is_equip = false;
    plain.is_jewel = false;
    plain.rarity = 0;
    plain.enhance_count = 0;
    plain.socket_total = 0;
    CHECK(!should_sell(plain, cfg));

    // 宝石物品不被装备规则命中（is_equip=false）。
    ItemView jewel = jewel_item();
    jewel.rarity = 0;
    jewel.enhance_count = 0;
    jewel.socket_total = 0;
    Config equip_only;
    equip_only.enabled = true;
    equip_only.rarity_enabled = true;
    equip_only.rarity_threshold = 4;
    equip_only.enhance_enabled = true;
    equip_only.enhance_threshold = 30;
    equip_only.socket_enabled = true;
    equip_only.socket_threshold = 15;
    CHECK(!should_sell(jewel, equip_only));

    // 装备物品不被宝石规则命中（is_jewel=false）。
    ItemView equip = equip_item();
    equip.jewel_tier = 0;
    Config jewel_only;
    jewel_only.enabled = true;
    jewel_only.gem_tier_enabled = true;
    jewel_only.gem_tier_threshold = 4;
    CHECK(!should_sell(equip, jewel_only));

    // 宝石物品本身可被宝石规则命中。
    CHECK(should_sell(jewel, jewel_only));
}

static void test_special_mask() {
    Config cfg;
    cfg.enabled = true;
    cfg.special_enabled = true;
    cfg.special_mask = autosell::kSpecialBackpack | autosell::kSpecialDice;

    // 命中勾选位之一即出售。
    ItemView item;
    item.special_types = autosell::kSpecialDice;
    CHECK(should_sell(item, cfg));
    item.special_types = autosell::kSpecialBackpack;
    CHECK(should_sell(item, cfg));
    item.special_types = autosell::kSpecialBackpack | autosell::kSpecialSealed;
    CHECK(should_sell(item, cfg));

    // 未勾选位不命中。
    item.special_types = autosell::kSpecialMercenarySeal;
    CHECK(!should_sell(item, cfg));
    item.special_types = autosell::kSpecialEnchantScroll | autosell::kSpecialSealed |
                         autosell::kSpecialItemBox;
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
    // 总开关开启但所有规则关闭：任何物品均不出售。
    Config cfg;
    cfg.enabled = true;

    ItemView equip = equip_item();
    equip.rarity = 0;
    equip.enhance_count = 0;
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
    cfg.rarity_enabled = true;
    cfg.rarity_threshold = 0;
    cfg.special_enabled = true;
    cfg.special_mask = autosell::kSpecialItemBox;

    ItemView equip = equip_item();
    equip.rarity = 4;  // 装备规则不命中
    equip.special_types = autosell::kSpecialItemBox;
    CHECK(should_sell(equip, cfg));

    // 两者均不命中 -> 不出售。
    equip.special_types = autosell::kSpecialDice;
    CHECK(!should_sell(equip, cfg));
}

int main() {
    test_master_switch_off();
    test_rarity_rule_boundaries();
    test_enhance_rule_boundaries();
    test_socket_rule_boundaries();
    test_gem_tier_rule_boundaries();
    test_equip_rules_or();
    test_disable_dimension();
    test_cross_category_independence();
    test_special_mask();
    test_no_rules_enabled();
    test_global_or_across_categories();
    std::printf("autosell_rules_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
