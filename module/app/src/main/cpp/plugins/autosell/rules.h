#pragma once

#include <cstdint>

#include "../../contracts/autosell.h"

// 自动出售纯规则引擎：只做「给定物品视图 + 配置 -> 是否出售」的判定。
// 零游戏/Android 依赖（纯 STL，编入 host 单测）；扫描、处置与 UI 由上层负责。
// 配置类型（autosell::Config / SpecialType）在 contracts/autosell.h（稳定端口，
// UI 消费者经该头访问，避免 feature→feature）。
//
// 语义依据 docs/archive/development/plugins/auto-sell.md §4.3、§4.4、§9：
//   - 总开关关闭恒不出售。
//   - 规则组取消各自开关：**用「值」当开关**——0 = 关闭；从 1 开始的整数为 1-based 档位，
//     出售阈值 = 值 - 1，比较方向统一 `<=`。
//   - 装备三规则之间为 AND：值 0=关、不参与判定；所有已开启
//     （值>0）的装备规则必须同时命中才命中；三条全关时不参与。
//   - 宝石规则同类内 OR：宝石满足任一已开启宝石规则即命中（装备以外保持 OR）。
//   - 跨类独立（OR）：装备规则只对 is_equip 生效；宝石规则只对 is_jewel 生效；特殊类型对任意物品生效；
//     命中任一类别即出售。
//   - 特殊类型仅三项：背包、普通徽章（勇士徽章 category 42..47）、骰子。
//   - 任一维度命中即出售（全局 OR）；未开启任何规则时不得出售。
//   - 硬保护（无条件、不受配置影响）：已强化（I_ENCHANT bits6-10>0）
//     或已镶嵌（I_SOCKET bits0-3>0）的装备永不出售，在规则判定之前先行排除。

namespace autosell {

// 物品视图（由上层从游戏内存/扩展袋读取后填充；本层不触碰游戏内存）。
struct ItemView {
    bool is_equip = false;
    int rarity = 0;         // 0..4
    // 强化筛选口径 = 剩余强化次数（UI 名称「强化耐久度」）；
    // 已强化次数与已镶嵌数仅供硬保护判定（>0 永不出售），不参与规则比较。
    int enhance_remaining = 0;  // 剩余强化次数 = (I_ENCHANT >> 2) & 0x0F
    int enhance_level = 0;      // 已强化次数 = (I_ENCHANT >> 6) & 0x1F（硬保护：>0 不售）
    int socket_total = 0;       // 总孔数 = (I_SOCKET >> 4) & 0x0F
    int socket_filled = 0;      // 已镶嵌数 = I_SOCKET & 0x0F（硬保护：>0 不售）
    bool is_jewel = false;
    int jewel_tier = 0;  // 0..4（category 28..32）
    // 宝石属性值在其随机范围内的百分位（0..100）；-1 = 未知/不适用
    // （探测不可用或失败）。gemRange 规则仅在此值 >= 0 时参与判定（fail-closed）。
    int jewel_percentile = -1;
    uint32_t special_types = 0;
};

enum class DecisionReason : uint8_t {
    kRuleMatch,
    kDisabled,
    kHardProtected,
    kNoRuleMatch,
};

// 判定单件物品是否应出售。纯函数，无副作用。
bool should_sell(const ItemView& item, const Config& cfg);
DecisionReason should_sell_with_reason(const ItemView& item, const Config& cfg);

}  // namespace autosell
