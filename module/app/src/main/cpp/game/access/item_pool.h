#pragma once

#include <cstdint>

// 物品对象池（ITEMPOOL）用量只读探测。
//
// 池布局来自 game_symbols.h：300 个 0x28 字节对象，对象 +0x1c bit0 = 占用。
// ITEMSYSTEM_CreateItem 只通过 ITEMPOOL_Allocate 的 NULL 返回表达“池满”，
// 而 ITEMSYSTEM_MakeItem 的完美道具分支不判空（实测 0x10c978 直接解引用 +8）。
// 因此需要在不写游戏状态的前提下读出占用数，用于把“池满 -> 创建失败”暴露到日志。
//
// 线程：只读、无副作用；池由游戏主线程分配/释放，调用方应在游戏线程读取。
namespace qol::game {

inline constexpr uint32_t kItemPoolCapacity = 300;

// 扫描池占用位。池指针未初始化或映射不可读时返回 false，out 归零。
bool item_pool_usage(uint32_t* out_used, uint32_t* out_capacity);

}  // namespace qol::game
