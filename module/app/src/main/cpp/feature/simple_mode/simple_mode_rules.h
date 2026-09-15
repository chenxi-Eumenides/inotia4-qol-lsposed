#pragma once

#include <cstdint>

// 简单模式纯逻辑层（不触内存、不调游戏函数、无平台依赖）：
// 只做「阵营 → 倍率」与「整数缩放」的决定，供 host test 穷举矩阵；
// 阵营判定本身在 simple_mode.cpp（需读游戏内存与调游戏函数）。
namespace simple_mode {

// 角色阵营归类结果。判定输入是查询结果而非角色指针，便于纯逻辑穷举。
enum class Side {
    kPlayer,   // 主角 / 队友 / 主控或队友的召唤物
    kMonster,  // C_TYPE==1：怪物（含敌方召唤物）
    kNeutral,  // NPC / 装饰物 / 宝箱泉水 / 无法判定
};

// 伤害缩放百分比（100 = 不变）。
//   victim 属玩家侧                          → 50   （受到的伤害 −50%）
//   否则 attacker 属玩家侧且 victim 是怪物    → 200  （打敌人的伤害 +100%）
//   其余（怪物互殴 / 涉及 NPC 与装饰物）      → 100
// 判定顺序固定：先看受害者，再看攻击者——保证「玩家打 NPC」「敌人打城镇 NPC」
// 都不会被误当成对敌输出。
int damage_percent(Side attacker, Side victim);

// 最大生命缩放百分比（100 = 不变）。
//   target 是怪物 → 50；其余 → 100。
int max_hp_percent(Side target);

// 按百分比缩放并做 int32 边界保护：
//   value <= 0 → 原样返回（非法值不放大）
//   percent == 100 → 原样返回
//   结果向上/向下取整后钳到 [1, INT32_MAX]（伤害 ≤0 会被游戏侧直接丢弃，
//   故缩放后至少保留 1 点）
std::int32_t scale_by_percent(std::int32_t value, int percent);

// 最大生命的半血结果：向下取整，下限 1（非法/极小值不会变成 0）。
std::int32_t halved_max_hp(std::int32_t max_hp);

}  // namespace simple_mode
