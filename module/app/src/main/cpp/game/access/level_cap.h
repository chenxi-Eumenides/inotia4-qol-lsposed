#pragma once

#include <cstdint>

// CONSTBASE 行 0 = 角色/佣兵经验等级上限（s16，原版 105）。等级字段本身是 u8，
// 因此可写范围 1..255；写入超过 255 会让升级循环的 level+1 在写字节时回绕。
namespace qol::game {

inline constexpr int32_t kLevelCapFieldMin = 1;
inline constexpr int32_t kLevelCapFieldMax = 255;

bool level_cap_ready();
// 读取行 0；表未装载或不可读返回 false。
bool level_cap_get(int32_t* out_cap);
// 写行 0；仅游戏主线程调用。越界或表未装载返回 false。
bool level_cap_set(int32_t cap);

}  // namespace qol::game
