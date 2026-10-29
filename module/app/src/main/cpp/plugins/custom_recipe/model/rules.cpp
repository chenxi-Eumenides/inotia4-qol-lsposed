#include "rules.h"

#include <cstring>
#include <limits>

namespace custom_recipe {

namespace {

QolResult rule_error(int32_t code, const char* message) {
    return qol::contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool valid_tier(Tier tier) {
    switch (tier) {
        case Tier::Grey:
        case Tier::White:
        case Tier::Green:
        case Tier::Blue:
        case Tier::Purple:
        case Tier::Gold:
            return true;
    }
    return false;
}

QolResult classify_value(ClassifyFn classify_fn, int32_t value, AttributeRange range,
                         void* context, Tier* out_tier) {
    if (classify_fn == nullptr || out_tier == nullptr) {
        return rule_error(kQolErrInvalidArgument, "missing quality classifier");
    }
    Tier tier = Tier::Grey;
    const QolResult result = classify_fn(value, range, context, &tier);
    if (result.ok != kQolOk) return result;
    if (!valid_tier(tier)) return rule_error(kQolErrInternal, "invalid quality tier");
    *out_tier = tier;
    return qol::contracts::ok();
}

}  // namespace

int tier_ordinal(Tier tier) {
    switch (tier) {
        case Tier::Grey:   return 0;
        case Tier::White:  return 1;
        case Tier::Green:  return 2;
        case Tier::Blue:   return 3;
        case Tier::Purple: return 4;
        case Tier::Gold:   return 5;
    }
    return 0;
}

Tier tier_from_ordinal(int ordinal) {
    if (ordinal <= 0) return Tier::Grey;
    if (ordinal >= 5) return Tier::Gold;
    return static_cast<Tier>(ordinal);
}

QolResult tier_lower_bound(Tier target, AttributeRange range, ClassifyFn classify_fn,
                           void* context, int32_t* out_value) {
    if (out_value == nullptr || classify_fn == nullptr || !valid_tier(target) ||
        range.max <= range.min) {
        return rule_error(kQolErrInvalidArgument, "invalid tier search input");
    }
    const int t = tier_ordinal(target);
    int64_t lo = range.min;
    int64_t hi = range.max;
    while (lo < hi) {
        const int64_t mid = lo + (hi - lo) / 2;
        Tier tier = Tier::Grey;
        const QolResult classified = classify_value(
            classify_fn, static_cast<int32_t>(mid), range, context, &tier);
        if (classified.ok != kQolOk) return classified;
        if (tier_ordinal(tier) >= t) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (lo < std::numeric_limits<int32_t>::min() || lo > std::numeric_limits<int32_t>::max()) {
        return rule_error(kQolErrInternal, "tier bound out of range");
    }
    *out_value = static_cast<int32_t>(lo);
    return qol::contracts::ok();
}

QolResult next_tier_interval(Tier current_tier, AttributeRange range, ClassifyFn classify_fn,
                             void* context, Tier* out_target, int32_t* out_lo,
                             int32_t* out_hi, bool* out_found) {
    if (out_lo == nullptr || out_hi == nullptr || out_found == nullptr ||
        classify_fn == nullptr || !valid_tier(current_tier) || range.max <= range.min) {
        return rule_error(kQolErrInvalidArgument, "invalid tier interval input");
    }
    *out_found = false;
    const int current_ordinal = tier_ordinal(current_tier);
    if (current_ordinal == tier_ordinal(Tier::Gold)) return qol::contracts::ok();

    // 金档恒有代表值 range.max；较窄范围中的空整数档会自然跳过。
    for (int ord = current_ordinal + 1; ord <= tier_ordinal(Tier::Gold); ++ord) {
        const Tier target = tier_from_ordinal(ord);
        int32_t lo = 0;
        QolResult result = tier_lower_bound(target, range, classify_fn, context, &lo);
        if (result.ok != kQolOk) return result;

        int64_t hi = range.max;
        if (ord != tier_ordinal(Tier::Gold)) {
            int32_t next_bound = 0;
            result = tier_lower_bound(tier_from_ordinal(ord + 1), range, classify_fn,
                                      context, &next_bound);
            if (result.ok != kQolOk) return result;
            hi = static_cast<int64_t>(next_bound) - 1;
        }
        if (static_cast<int64_t>(lo) <= hi) {
            if (out_target != nullptr) *out_target = target;
            *out_lo = lo;
            *out_hi = static_cast<int32_t>(hi);
            *out_found = true;
            return qol::contracts::ok();
        }
    }
    return qol::contracts::ok();
}

QolResult compute_tier_up_value(Tier current_tier, AttributeRange range,
                                ClassifyFn classify_fn, void* context,
                                RandomInclusiveFn rand_inclusive, UpgradeResult* out_result,
                                Tier* out_target, int32_t* out_new_value) {
    if (out_result == nullptr || !valid_tier(current_tier)) {
        return rule_error(kQolErrInvalidArgument, "invalid tier upgrade input");
    }
    *out_result = UpgradeResult::kDegenerate;
    if (range.max <= range.min) return qol::contracts::ok();
    if (classify_fn == nullptr) {
        return rule_error(kQolErrInvalidArgument, "missing quality classifier");
    }
    if (current_tier == Tier::Gold) {
        *out_result = UpgradeResult::kAlreadyGold;
        return qol::contracts::ok();
    }
    if (rand_inclusive == nullptr) {
        return rule_error(kQolErrInvalidArgument, "missing random source");
    }

    Tier target = Tier::Grey;
    int32_t lo = 0;
    int32_t hi = 0;
    bool found = false;
    const QolResult interval_result = next_tier_interval(
        current_tier, range, classify_fn, context, &target, &lo, &hi, &found);
    if (interval_result.ok != kQolOk) return interval_result;
    if (!found) {
        *out_result = UpgradeResult::kAlreadyGold;
        return qol::contracts::ok();
    }

    int32_t value = 0;
    const QolResult random_result = rand_inclusive(lo, hi, &value);
    if (random_result.ok != kQolOk) return random_result;
    if (value < lo || value > hi) {
        return rule_error(kQolErrInternal, "random source returned value outside interval");
    }
    if (out_target != nullptr) *out_target = target;
    if (out_new_value != nullptr) *out_new_value = value;
    *out_result = UpgradeResult::kOk;
    return qol::contracts::ok();
}

int jewel_grade_from_category(int category) {
    if (category < kJewelCategoryFirst || category > kJewelCategoryLast) return 0;
    return category - kJewelCategoryFirst + 1;
}

int equipment_grade_from_rarity(int rarity) {
    if (rarity < 0 || rarity > 4) return 0;
    return rarity + 1;
}

bool affix_upgradable(int32_t status, int32_t level, AttributeRange range) {
    return status == kQolOk && level >= 0 && level < 5 && range.max > range.min;
}

int material_count_for(int base_count, int grade, int level) {
    // 零费等级阈值：kFreeStartLevel = 最低底价组合（base+grade=2）恰好免费的等级；
    // kFreeFullLevel = 最高底价组合（base+grade=10）恰好免费的等级。
    const int kFreeStartLevel = 25;
    const int kFreeFullLevel = 95;
    // 折扣额度端点：kFreeStartLevel 级时折扣 = 最低底价、kFreeFullLevel 级时折扣 = 最高底价。
    // 改动阈值时须与档位 1..5 推导出的底价范围 [2,10] 两端对齐，否则端点档位的零费等级会偏离预期。
    const int kDiscountAtStart = 2;
    const int kDiscountAtFull = 10;

    if (base_count < 1 || grade < 1) return 0;
    int lv = level;
    if (lv < 0) lv = 0;
    if (lv > kMaxCharacterLevel) lv = kMaxCharacterLevel;

    // 消耗 = max(0, base_count + grade − F)，
    // F = kDiscountAtStart + floor((lv − kFreeStartLevel) × (kDiscountAtFull − kDiscountAtStart)
    //                              / (kFreeFullLevel − kFreeStartLevel))。
    // C++ 整数除法向零取整；被除数为负时须修正为向下取整，否则低等级折扣偏大、零费等级提前。
    const int span = kFreeFullLevel - kFreeStartLevel;
    const int scaled = (lv - kFreeStartLevel) * (kDiscountAtFull - kDiscountAtStart);
    int discount = scaled / span;
    if (scaled < 0 && scaled % span != 0) --discount;
    discount += kDiscountAtStart;

    const int price = base_count + grade;
    if (price <= discount) return 0;
    return price - discount;
}

bool slot_add_allowed(int placed_slots, int stack_count) {
    if (placed_slots < 0 || stack_count <= 0) return false;
    // 每格代表 1 个单位：已占格数必须严格小于堆内数量才能再占一格。
    return placed_slots < stack_count;
}

bool stack_units_available(int units, int stack_count) {
    if (units <= 0) return true;
    return stack_count >= units;
}

bool execute_token_valid(bool enabled, uint32_t pending_token, uint32_t current_token) {
    if (!enabled) return false;
    return pending_token != 0 && pending_token == current_token;
}

bool table_invalid_log_needed(uint8_t recipe_size, uint8_t mixture_size, uint16_t count,
                              uint8_t last_recipe_size, uint8_t last_mixture_size,
                              uint16_t last_count) {
    return recipe_size != last_recipe_size || mixture_size != last_mixture_size ||
           count != last_count;
}

int scaled_jewel_value(int value, uint16_t scale_permille) {
    if (value <= 0) return 0;
    // 0 视为未配置：fail-closed 返回原值，绝不把数值清零。
    if (scale_permille == 0 || scale_permille == 1000) {
        return value > kJewelValueMax ? kJewelValueMax : value;
    }
    // **向上取整**：ceil(value × permille / 1000)。纯整数实现（+999 后整除），避免浮点端点漂移。
    const long long numer =
        static_cast<long long>(value) * static_cast<long long>(scale_permille);
    const long long scaled = (numer + 999) / 1000;
    if (scaled <= 0) return 0;
    return scaled > kJewelValueMax ? kJewelValueMax : static_cast<int>(scaled);
}

}  // namespace custom_recipe
