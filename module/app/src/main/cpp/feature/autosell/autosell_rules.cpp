#include "feature/autosell/autosell_rules.h"

namespace autosell {

namespace {

// 装备类规则：任一已启用规则命中即真（同类内 OR）。
bool equip_rule_hit(const ItemView& item, const Config& cfg) {
    if (cfg.rarity_enabled && item.rarity <= cfg.rarity_threshold) {
        return true;
    }
    if (cfg.enhance_enabled && item.enhance_count <= cfg.enhance_threshold) {
        return true;
    }
    if (cfg.socket_enabled && item.socket_total <= cfg.socket_threshold) {
        return true;
    }
    return false;
}

// 宝石类规则：任一已启用规则命中即真（同类内 OR）。
bool jewel_rule_hit(const ItemView& item, const Config& cfg) {
    return cfg.gem_tier_enabled && item.jewel_tier <= cfg.gem_tier_threshold;
}

// 特殊类型规则：勾选集合与物品位标志有交集即真（对任意物品生效）。
bool special_rule_hit(const ItemView& item, const Config& cfg) {
    return cfg.special_enabled && cfg.special_mask != 0u &&
           (item.special_types & cfg.special_mask) != 0u;
}

}  // namespace

bool should_sell(const ItemView& item, const Config& cfg) {
    // 总开关关闭：任何规则均不生效。
    if (!cfg.enabled) {
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
