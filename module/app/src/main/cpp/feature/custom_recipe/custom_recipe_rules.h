#pragma once

#include <cstdint>

#include "feature/attribute_range/attribute_range.h"

// 自定义合成配方「提升一档」纯逻辑（custom-craft-recipe §4.6 / §2.4）。
//
// 档位区间必须与 attr_range::classify 严格互逆：本模块用 classify 作为唯一真值判定
// 某整数落在哪一档，并以二分查找求出每一档在闭区间 [min,max] 内的代表值区间；
// 不使用浮点、不写死百分位，从而杜绝端点漂移。
namespace custom_recipe {

// 「提升一档」结果状态。
enum class UpgradeResult : uint8_t {
    kOk,           // 已算出新数值（落在高一档区间内）
    kAlreadyGold,  // 当前已是金档，不可再升
    kDegenerate,   // 区间退化（max <= min）无法判定档位
};

// 档位序数（低→高）：Grey=0, White=1, Green=2, Blue=3, Purple=4, Gold=5。
int tier_ordinal(attr_range::Tier tier);

// 由序数还原档位（0..5）；越界钳制到 [Grey, Gold]。
attr_range::Tier tier_from_ordinal(int ordinal);

// 求目标档位在闭区间 [min,max] 内的最小代表值：
//   smallest v ∈ [min,max] 使 ordinal(classify(v,min,max)) >= ordinal(target)。
// 依赖 classify 对 v 单调不减，二分查找实现，边界严格互逆于 classify。
// 前置：max > min。target==Gold 时返回 max。
int tier_lower_bound(attr_range::Tier target, int min, int max);

// 求「当前数值所在档的上一档」代表闭区间 [out_lo,out_hi]（升序，out_lo<=out_hi）。
// 若紧邻上一档在 [min,max] 内无整数代表值（区间过窄），继续向上走到下一个有代表值
// 的档（金档恒有代表值 max），因此结果档位恒严格高于当前档。
// out_target（可空）回传区间所属目标档。
// 返回 false：max<=min（退化）或当前已金档（无可升目标）。
bool next_tier_interval(int current_value, int min, int max,
                        attr_range::Tier* out_target, int* out_lo, int* out_hi);

// 在 next_tier_interval 得到的闭区间内用 rand_inclusive 取新数值。
// rand_inclusive(lo,hi) 须在闭区间 [lo,hi] 均匀返回整数（生产用 MATH_GetRandom）。
// 取回值钳制进 [lo,hi]，故 classify(新值)==out_target（若 out_target 非空则回传）。
// 退化/已金档 → 对应 UpgradeResult，*out_new_value 不写。
UpgradeResult compute_tier_up_value(int current_value, int min, int max,
                                    int (*rand_inclusive)(int, int),
                                    attr_range::Tier* out_target, int* out_new_value);

// ---------------------------------------------------------------------------
// 材料需求数量（custom-craft-recipe §4.7 / CountRule::kJewelGradeAndLevel）
// ---------------------------------------------------------------------------

// 宝石类别（item+8 的 bits6-15）范围：28..32，与 ITEMSYSTEM_IsJewel 一致。
constexpr int kJewelCategoryFirst = 28;
constexpr int kJewelCategoryLast = 32;
// 角色等级上限（EXP 表 105 级）。
constexpr int kMaxCharacterLevel = 105;

// 宝石类别 → 档位 1..5：28=低级宝石、29=中级、30=高级、31=顶级、32=混沌。
// 不在 [28,32] 内返回 0（调用方应据此放弃改写需求数，fail-closed）。
int jewel_grade_from_category(int category);

// 材料需求数量 = ceil(base_count × grade × (kMaxCharacterLevel − level) / kMaxCharacterLevel)。
// 纯整数运算，无浮点。level 钳到 [0,105]；base_count<1 或 grade<1 → 返回 0 由调用方处理。
// 边界：level=105 → 0（满级不耗材料）；level≤1 且 grade=5 → 5；grade=1、level=1 → 1。
int material_count_for(int base_count, int grade, int level);

}  // namespace custom_recipe
