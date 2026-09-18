#pragma once

#include <cstdint>

// 游戏层：属性范围详情渲染所需的符号/GOT 原语（ARCHITECTURE.md §5「版本耦合唯一收口」）。
// provider 通过 Game Host 调用这些原语；符号常量与 fn_* 指针收口在 game/。
//
// 线程纪律：以下函数只在游戏主线程（详情渲染 / 帧回调）调用。

namespace qol::game {

// 物品类别 = item+I_TYPE 的 bits6-15；空 item 返回 -1。
int attr_item_category(const void* item);

// 类别是否走 ITEMSTATICOPTBASE 固定词缀表（ITEMDATABASE[cat].+6 bit1）。
bool attr_item_uses_fixed_option_table(int category);

// 固定词缀表查询 (类别, 词缀索引) -> (p1, p2) s8 百分比；未命中/未就绪返回 false。
bool attr_fixed_option_percent_pair(int category, int option_index, int* out_p1, int* out_p2);

// 物品能力等级（fn_item_get_ability_level）；未解析返回 0。
int attr_ability_level(const void* item);

// category 是否独立宝石（fn_is_jewel）；未解析返回 false。
bool attr_is_jewel_category(int category);

// 探测装备词缀掷值区间（借已安装的 MATH_GetRandom wrapper 截获，不消耗游戏 RNG）。
bool attr_call_option_value(int option_index, int level, int flag, void* item, int* out_value);

// 探测独立宝石属性随机区间 [X, 2X]。
bool attr_call_jewel_value(int type, void* item, int* out_value);

// 原函数备份登记（宿主 on_resource_ready 交还）。
// MATH_GetRandom 的备份与捕获状态由 provider 私有持有。

// MATH_GetRandom 原函数是否已就绪（未就绪时调用探测会真实消耗 RNG）。

// 原函数备份读取（wrapper 内使用）；未登记返回 nullptr。

// MATH_GetRandom wrapper 主体：捕获窗口命中则返回上界（不消耗 RNG），否则调原函数。

// 在详情文本 insert 位置插入 "$B$<code>"；越界/找不到数值锚点返回 false。
bool attr_insert_value_color(char* before, char* after, char code);

}  // namespace qol::game
