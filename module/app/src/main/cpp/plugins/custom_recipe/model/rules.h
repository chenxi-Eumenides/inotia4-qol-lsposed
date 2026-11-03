#pragma once

#include <cstdint>

#include "../../../contracts/attribute_quality.h"

// 自定义合成配方「提升一档」纯逻辑（custom-craft-recipe §4.6 / §2.4）。
//
// 候选档位均由 attribute.quality v2 evaluate 计算；本层仅搜索其单调档位边界。
namespace custom_recipe {

using Tier = qol::contracts::AttributeQualityTier;
using AttributeRange = qol::contracts::AttributeRange;
using ClassifyFn = QolResult (*)(int32_t value, AttributeRange range, void* context, Tier* out);
using RandomInclusiveFn = QolResult (*)(int32_t lo, int32_t hi, int32_t* out);

// 「提升一档」结果状态。
enum class UpgradeResult : uint8_t {
    kOk,           // 已算出新数值（落在高一档区间内）
    kAlreadyGold,  // 当前已是金档，不可再升
    kDegenerate,   // 区间退化（max <= min）无法判定档位
};

// 档位序数（低→高）：Grey=0, White=1, Green=2, Blue=3, Purple=4, Gold=5。
int tier_ordinal(Tier tier);

// 由序数还原档位（0..5）；越界钳制到 [Grey, Gold]。
Tier tier_from_ordinal(int ordinal);

// 求目标档位在闭区间内的最小代表值。分类失败经 QolResult 原样返回。
QolResult tier_lower_bound(Tier target, AttributeRange range, ClassifyFn classify_fn,
                           void* context, int32_t* out_value);

// 求当前档之上的首个有代表值档位及其闭区间；窄范围跳过无整数代表值的中间档。
// 已是 Gold 时成功返回 found=false；范围退化或分类失败通过 QolResult 表达。
QolResult next_tier_interval(Tier current_tier, AttributeRange range, ClassifyFn classify_fn,
                             void* context, Tier* out_target, int32_t* out_lo,
                             int32_t* out_hi, bool* out_found);

// 在目标档区间内随机取值；UpgradeResult 仅表示既有业务状态，分类错误由 QolResult 返回。
QolResult compute_tier_up_value(Tier current_tier, AttributeRange range, ClassifyFn classify_fn,
                                void* context, RandomInclusiveFn rand_inclusive,
                                UpgradeResult* out_result, Tier* out_target,
                                int32_t* out_new_value);

// ---------------------------------------------------------------------------
// 材料需求数量（custom-craft-recipe §4.7 / CountRule::kQualityAndAttributeTier）
// ---------------------------------------------------------------------------

// 宝石类别（item+8 的 bits6-15）范围：28..32，与 ITEMSYSTEM_IsJewel 一致。
constexpr int kJewelCategoryFirst = 28;
constexpr int kJewelCategoryLast = 32;
// 角色等级上限（EXP 表 105 级）。
constexpr int kMaxCharacterLevel = 105;

// 宝石类别 → 品质档位 0..4：28=低级宝石、29=中级、30=高级、31=顶级、32=混沌。
// 不在 [28,32] 内返回 -1（调用方应据此放弃改写需求数，fail-closed）。
int jewel_quality_tier_from_category(int category);

// 装备稀有度（ITEMSYSTEM_GetRarity 0..4 = 白/绿/蓝/黄/紫）→ 品质档位 0..4；越界返回 -1。
int equipment_quality_tier_from_rarity(int rarity);

// 词条可升阶：质量查询 status==kQolOk、level 在 [0,4]、区间非退化（max>min）。
bool affix_upgradable(int32_t status, int32_t level, AttributeRange range);

// 属性等阶 = attribute.quality 的 level：0%→0、30%→1、60%→2、75%→3、90%→4。
// level 5（金档，不可升阶）或越界 → -1（调用方 fail-closed）。
int attribute_tier_from_quality_level(int quality_level);

// 材料需求数量 = max(0, quality_tier + attribute_tier − F)，
// F = floor((clamp(character_level, 0, 105) − 25) × 8 / 70)。
// quality_tier 为品质档位（宝石类别 / 装备稀有度 → 0..4）、attribute_tier 为属性等阶
// （attribute.quality 的 level，见 attribute_tier_from_quality_level）。
// 端点：character_level=25 时 F=0，最低组合「品质0+等阶0」恰好免费；
// character_level=95 时 F=8，最高组合「品质4+等阶4」恰好免费。
// floor 为向 −∞ 取整；C++ 整数除法向零，被除数为负时须修正（见实现），否则低等级折扣偏大、
// 零费等级提前。
// quality_tier / attribute_tier 不在 0..4 → 返回 -1（调用方据此保持原需求数，不会变成免费合成）。
// 同一 level 下非零消耗随品质+等阶之和严格 +1 递增（如 25 级 [0,1,2,3,4]、43 级 [0,0,0,1,2]）。
int material_count_for(int quality_tier, int attribute_tier, int character_level);

// ---- 3 格隐式配方的放料/扣料数量判定（纯逻辑，host 可测；需求见 §2.8）----
// 语义：**每格 = 1 件**。不可堆叠物品 = 1 个对象（同一对象不得占两格）；
// 可堆叠物品 = 1 个单位（同一个堆可以占多格，受**类别持有总数**约束：「2 个物品不能添加 3 次」）。

// 可堆叠物品：该**类别**已被占 placed_slots 格、当前持有总数 held_count 个 → 是否还允许再占一格。
// placed_slots < 0 或 held_count <= 0 → false（fail-closed）。
bool slot_add_allowed(int placed_slots, int held_count);

// 合成前复核：该**类别**本次需扣 units 个单位、当前持有总数 held_count 个 → 是否足够。
// units <= 0 → true（该类别无需扣减）；held_count 不足或为负 → false（调用方须中止且不消耗）。
bool stack_units_available(int units, int held_count);

// 确认框回调（three_slot_execute）的 pending/token 校验（纯逻辑）。
// enabled=false（总开关停用）、pending_token==0（无待确认条目）、或
// pending_token != current_token（停用/表重载已使该确认失效）→ false；
// 调用方按「格子已变」（kStaleSlots）处理：不消耗任何材料、不产出、不扣费。
bool execute_token_valid(bool enabled, uint32_t pending_token, uint32_t current_token);

// 表注入「全局未就绪」日志去重（纯逻辑）：尺寸/计数与上次不同才需记录，避免逐帧重试刷屏。
// 首次调用时 last_* 传哨兵（如 0xFF/0xFFFF）即可保证记录一条。
bool table_invalid_log_needed(uint8_t recipe_size, uint8_t mixture_size, uint16_t count,
                              uint8_t last_recipe_size, uint8_t last_mixture_size,
                              uint16_t last_count);

// ---- 宝石数值缩放（§2.8「宝石 + 混沌武器卷轴 + 混沌防具卷轴」配方）----

// 宝石数值位的上限（= game_symbols.h 的 JEWEL_VALUE_MASK，item+0x10 的 bits0-10）。
// 本常量在纯逻辑层独立声明（规则层不引入游戏符号头），两者由 §2.8 文档与真机验收保持同步。
constexpr int kJewelValueMax = 2047;

// 新数值 = **ceil**(value × scale_permille / 1000)（向上取整），再钳到 [0, kJewelValueMax]。
//   scale_permille == 1000 → 原值；1200 → ×1.2（向上取整）；0 → 原值（视为未配置，fail-closed，
//   避免把数值清零）；value <= 0 → 0。
// 纯整数运算，无浮点。
int scaled_jewel_value(int value, uint16_t scale_permille);

}  // namespace custom_recipe
