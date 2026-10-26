#include "rules.h"

namespace autosell {

namespace {

// 装备类规则：三条规则之间为 AND——值 0=关、不参与判定；
// 所有已开启（值 v>0）的规则必须同时命中才为真；三条全关时不参与（返回 false）。
// 比较方向统一 `<=`：值 v → 阈值 v-1。
bool equip_rule_hit(const ItemView& item, const Config& cfg) {
    bool any_enabled = false;

    if (cfg.rarity > 0) {
        any_enabled = true;
        if (item.rarity < 0) {
            return false;
        }
        if (item.rarity > cfg.rarity - 1) {
            return false;
        }
    }
    // 强化口径：比较「剩余强化次数」enhance_remaining（bits2-5），
    // 界面名称「强化耐久度」；不再读已强化次数 bits6-10（该字段改由硬保护使用）。
    if (cfg.enhance > 0) {
        any_enabled = true;
        if (item.enhance_remaining > cfg.enhance - 1) {
            return false;
        }
    }
    if (cfg.socket > 0) {
        any_enabled = true;
        if (item.socket_total > cfg.socket - 1) {
            return false;
        }
    }
    return any_enabled;
}

// 宝石属性档位 -> 属性百分位阈值（1→30 / 2→60 / 3→75 / 4→90 / 5→99）。
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

// 背包、徽章、骰子分别按用户选择的最大可售档位判定。
bool tier_rule_hit(const ItemView& item, const Config& cfg) {
    if (cfg.backpack_tier > 0 && item.is_backpack && item.category >= 1 &&
        item.category <= cfg.backpack_tier) return true;
    if (cfg.badge_tier > 0 && item.badge_rank > 0 && item.badge_rank <= cfg.badge_tier)
        return true;
    if (cfg.dice_tier > 0 && item.dice_tier > 0 && item.dice_tier <= cfg.dice_tier)
        return true;
    return false;
}

}  // namespace

bool should_sell(const ItemView& item, const Config& cfg) {
    return should_sell_with_reason(item, cfg) == DecisionReason::kRuleMatch;
}

DecisionReason should_sell_with_reason(const ItemView& item, const Config& cfg) {
    // 总开关关闭：任何规则均不生效。
    if (!cfg.enabled) {
        return DecisionReason::kDisabled;
    }

    // 硬保护：已强化或已镶嵌的装备永不出售。
    // 已强化 = I_ENCHANT bits6-10 > 0；已镶嵌 = I_SOCKET bits0-3 > 0。
    // 无条件、不受配置影响，在一切规则判定（含宝石/特殊类型）之前先行排除。
    if (item.enhance_level > 0 || item.socket_filled > 0) {
        return DecisionReason::kHardProtected;
    }

    // 装备规则只作用于装备；宝石规则只作用于宝石；两类相互独立。
    if (item.is_equip && equip_rule_hit(item, cfg)) {
        return DecisionReason::kRuleMatch;
    }
    if (item.is_jewel && jewel_rule_hit(item, cfg)) {
        return DecisionReason::kRuleMatch;
    }
    if (tier_rule_hit(item, cfg)) {
        return DecisionReason::kRuleMatch;
    }

    // 未启用任何规则（或均未命中）时不出售。
    return DecisionReason::kNoRuleMatch;
}

}  // namespace autosell
