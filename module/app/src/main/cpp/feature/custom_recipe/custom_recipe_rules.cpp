#include "custom_recipe_rules.h"

namespace custom_recipe {

using attr_range::classify;
using attr_range::Tier;

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

int tier_lower_bound(Tier target, int min, int max) {
    // classify(v,min,max) 对 v 单调不减（v<min→Grey，[min,max)→按百分位升档，>=max→Gold），
    // 故二分查找首个 ordinal(classify(v)) >= 目标序数 的 v。
    const int t = tier_ordinal(target);
    int lo = min;
    int hi = max;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (tier_ordinal(classify(mid, min, max)) >= t) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

bool next_tier_interval(int current_value, int min, int max, Tier* out_target,
                        int* out_lo, int* out_hi) {
    if (max <= min) return false;
    const int current_ordinal = tier_ordinal(classify(current_value, min, max));
    if (current_value >= max && current_ordinal == tier_ordinal(Tier::Gold)) {
        return false;  // 已金档（含金值 >= max）
    }
    // 紧邻上一档若在 [min,max] 内无整数代表值（区间过窄），继续向上找有代表值的档；
    // 金档 [max,max] 恒有代表值，故循环必在 ord<=5 内命中。
    for (int ord = current_ordinal + 1; ord <= tier_ordinal(Tier::Gold); ++ord) {
        const Tier target = tier_from_ordinal(ord);
        const int lo = tier_lower_bound(target, min, max);
        const int hi = (ord == tier_ordinal(Tier::Gold))
                           ? max
                           : tier_lower_bound(tier_from_ordinal(ord + 1), min, max) - 1;
        if (lo <= hi) {
            if (out_target != nullptr) *out_target = target;
            *out_lo = lo;
            *out_hi = hi;
            return true;
        }
    }
    return false;
}

UpgradeResult compute_tier_up_value(int current_value, int min, int max,
                                    int (*rand_inclusive)(int, int), Tier* out_target,
                                    int* out_new_value) {
    if (max <= min) return UpgradeResult::kDegenerate;
    if (tier_ordinal(classify(current_value, min, max)) == tier_ordinal(Tier::Gold)) {
        return UpgradeResult::kAlreadyGold;
    }
    Tier target = Tier::Grey;
    int lo = 0;
    int hi = 0;
    if (!next_tier_interval(current_value, min, max, &target, &lo, &hi)) {
        return UpgradeResult::kAlreadyGold;
    }
    if (rand_inclusive == nullptr) return UpgradeResult::kDegenerate;
    int value = rand_inclusive(lo, hi);
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    if (out_target != nullptr) *out_target = target;
    if (out_new_value != nullptr) *out_new_value = value;
    return UpgradeResult::kOk;
}

int jewel_grade_from_category(int category) {
    if (category < kJewelCategoryFirst || category > kJewelCategoryLast) return 0;
    return category - kJewelCategoryFirst + 1;
}

int material_count_for(int base_count, int grade, int level) {
    if (base_count < 1 || grade < 1) return 0;
    int lv = level;
    if (lv < 0) lv = 0;
    if (lv > kMaxCharacterLevel) lv = kMaxCharacterLevel;
    // ceil(base × grade × (105 − lv) / 105)，全程整数，避免浮点端点漂移。
    const long long numer =
        static_cast<long long>(base_count) * static_cast<long long>(grade) *
        static_cast<long long>(kMaxCharacterLevel - lv);
    if (numer <= 0) return 0;
    return static_cast<int>((numer + kMaxCharacterLevel - 1) / kMaxCharacterLevel);
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
