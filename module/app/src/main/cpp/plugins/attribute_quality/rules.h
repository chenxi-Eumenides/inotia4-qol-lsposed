#pragma once

#include <cstdint>

#include "contracts/attribute_quality.h"

namespace attribute_quality {

using Tier = qol::contracts::AttributeQualityTier;

// 按 [min, max] 闭区间内的百分位分档（整数运算）：
//  1) max <= min              -> Gold
//  2) value >= max            -> Gold（含超出范围的特殊/固定值，视为满值）
//  3) pct = (value - min) * 100 / (max - min)   // 向下取整
//     pct >= 90 -> Purple；>= 75 -> Blue；>= 60 -> Green；>= 30 -> White；其余（含 value < min 的负 pct）-> Grey
Tier classify(int value, int min, int max);

// 按 [min, max] 闭区间计算百分位（整数运算，返回值恒在 0..100）：
//  1) max <= min              -> 100（退化区间无法计算，统一视为满值）
//  2) value >= max            -> 100（含超出范围的特殊/固定值，视为满值）
//  3) value < min             -> 0
//  4) pct = (value - min) * 100 / (max - min)   // 向下取整
int percentile(int value, int min, int max);

// 固定词缀表（ITEMSTATICOPTBASE）驱动的范围：值域为 [base + base*p1/100, base + base*p2/100]。
//  p1/p2 为记录中的 s8 百分比，除法向零截断，与游戏 ITEMSYSTEM_CreateItem 0x10c2a4-0x10c304 的
//  smull/lsr 序列一致；base <= 0 时游戏取 1（同处 csinc），此处保持一致。
//  p1 == p2 时退化为单值区间，由 classify/percentile 按满值处理。
void fixed_option_range(int base, int p1, int p2, int* out_min, int* out_max);

}  // namespace attribute_quality
