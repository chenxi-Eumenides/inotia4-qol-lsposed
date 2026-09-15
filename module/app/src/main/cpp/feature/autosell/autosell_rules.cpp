#include "feature/autosell/autosell_rules.h"

namespace autosell {

namespace {

// 装备类规则：任一已开启规则命中即真（同类内 OR）。值 0=关；值 v → 阈值 v-1。
bool equip_rule_hit(const ItemView& item, const Config& cfg) {
    if (cfg.rarity > 0 && item.rarity <= cfg.rarity - 1) {
        return true;
    }
    // 强化口径（用户 2026-09-15 裁决）：比较「剩余强化次数」enhance_remaining（bits2-5），
    // 界面名称「强化耐久度」；不再读已强化次数 bits6-10（该字段改由硬保护使用）。
    if (cfg.enhance > 0 && item.enhance_remaining <= cfg.enhance - 1) {
        return true;
    }
    if (cfg.socket > 0 && item.socket_total <= cfg.socket - 1) {
        return true;
    }
    return false;
}

// 宝石属性范围档位 -> 属性百分位阈值（1→30 / 2→60 / 3→75 / 4→90 / 5→99）。
// 最高档取 99：即使调到最大也不出售满分（百分位 100）的宝石。
// 越界/0 返回 -1 = 关闭（不命中），避免未知档位默认命中。
int gem_range_threshold(int gem_range) {
    switch (gem_range) {
        case 1:
            return 30;
        case 2:
            return 60;
        case 3:
            return 75;
        case 4:
            return 90;
        case 5:
            return 99;
        default:
            return -1;
    }
}

// 宝石类规则：任一已开启规则命中即真（同类内 OR）。值 0=关；值 v → 阈值 v-1。
bool jewel_rule_hit(const ItemView& item, const Config& cfg) {
    if (cfg.gem_tier > 0 && item.jewel_tier <= cfg.gem_tier - 1) {
        return true;
    }
    // 属性范围规则：出售百分位 <= 阈值 的低品质宝石。percentile < 0（探测不可用/失败）
    // 时不命中（fail-closed，不得默认 0 触发误售）。
    const int threshold = gem_range_threshold(cfg.gem_range);
    if (threshold >= 0 && item.jewel_percentile >= 0 && item.jewel_percentile <= threshold) {
        return true;
    }
    return false;
}

// 特殊类型规则：掩码非 0 且勾选集合与物品位标志有交集即真（对任意物品生效）。
bool special_rule_hit(const ItemView& item, const Config& cfg) {
    return cfg.special_mask != 0u && (item.special_types & cfg.special_mask) != 0u;
}

}  // namespace

bool should_sell(const ItemView& item, const Config& cfg) {
    // 总开关关闭：任何规则均不生效。
    if (!cfg.enabled) {
        return false;
    }

    // 硬保护：已强化或已镶嵌的装备永不出售（用户 2026-09-15）。
    // 已强化 = I_ENCHANT bits6-10 > 0；已镶嵌 = I_SOCKET bits0-3 > 0。
    // 无条件、不受配置影响，在一切规则判定（含宝石/特殊类型）之前先行排除。
    if (item.enhance_level > 0 || item.socket_filled > 0) {
        return false;
    }

    // 装备规则只作用于装备；宝石规则只作用于宝石；两类相互独立。
    if (item.is_equip && equip_rule_hit(item, cfg)) {
        return true;
    }
    if (item.is_jewel && jewel_rule_hit(item, cfg)) {
        return true;
    }
    if (special_rule_hit(item, cfg)) {
        return true;
    }

    // 未启用任何规则（或均未命中）时不出售。
    return false;
}

}  // namespace autosell
