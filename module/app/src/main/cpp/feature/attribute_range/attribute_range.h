#pragma once

namespace attr_range {

// 颜色档位（由低到高）
enum class Tier { Grey, White, Green, Blue, Purple, Gold };

// 档位 -> 游戏内联文本颜色码（用于 "$<code>文本$B" 的 code 字符）
// Grey='G', White='W', Green='C', Blue='L', Purple='V', Gold='Y'
char color_code(Tier tier);

// 按 [min, max] 闭区间内的百分位分档（整数运算）：
//  1) max <= min              -> Gold
//  2) value >= max            -> Gold（含超出范围的特殊/固定值，视为满值）
//  3) pct = (value - min) * 100 / (max - min)   // 向下取整
//     pct >= 90 -> Purple；>= 75 -> Blue；>= 60 -> Green；>= 30 -> White；其余（含 value < min 的负 pct）-> Grey
Tier classify(int value, int min, int max);

}  // namespace attr_range
