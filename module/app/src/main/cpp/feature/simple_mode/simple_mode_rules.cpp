#include "feature/simple_mode/simple_mode_rules.h"

#include <limits>

namespace simple_mode {

int damage_percent(Side attacker, Side victim) {
    // 先判受害者：玩家侧受到的伤害一律减半，与本模式外的攻击者身份无关。
    if (victim == Side::kPlayer) {
        return 50;
    }
    // 再判攻击者：只有「玩家侧打怪物」才加伤；打 NPC/装饰物不加（它们不是敌人）。
    if (attacker == Side::kPlayer && victim == Side::kMonster) {
        return 200;
    }
    return 100;
}

int max_hp_percent(Side target) {
    return target == Side::kMonster ? 50 : 100;
}

std::int32_t scale_by_percent(std::int32_t value, int percent) {
    if (value <= 0 || percent == 100) {
        return value;
    }
    const std::int64_t scaled = static_cast<std::int64_t>(value) * percent / 100;
    if (scaled > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (scaled < 1) {
        return 1;
    }
    return static_cast<std::int32_t>(scaled);
}

std::int32_t halved_max_hp(std::int32_t max_hp) {
    if (max_hp <= 2) {
        return 1;
    }
    return max_hp / 2;
}

}  // namespace simple_mode
