#pragma once

#include <cstdint>

// 自动出售纯规则引擎：只做「给定物品视图 + 配置 -> 是否出售」的判定。
// 零游戏/Android 依赖（纯 STL，编入 host 单测）；扫描、处置与 UI 由上层负责。
//
// 语义依据 docs/development/features/auto-sell.md §4.3、§4.4、§9（用户 2026-09-14 裁决）：
//   - 总开关关闭恒不出售。
//   - 规则组取消各自开关：**用「值」当开关**——0 = 关闭；从 1 开始的整数为 1-based 档位，
//     出售阈值 = 值 - 1，比较方向统一 `<=`。
//   - 装备三规则之间为 AND（用户 2026-09-16 裁决）：值 0=关、不参与判定；所有已开启
//     （值>0）的装备规则必须同时命中才命中；三条全关时不参与。
//   - 宝石规则同类内 OR：宝石满足任一已开启宝石规则即命中（用户 2026-09-16：装备以外保持 OR）。
//   - 跨类独立（OR）：装备规则只对 is_equip 生效；宝石规则只对 is_jewel 生效；特殊类型对任意物品生效；
//     命中任一类别即出售。
//   - 特殊类型仅三项：背包、普通徽章（勇士徽章 category 42..47）、骰子。
//   - 任一维度命中即出售（全局 OR）；未开启任何规则时不得出售。
//   - 硬保护（用户 2026-09-15 裁决，无条件、不受配置影响）：已强化（I_ENCHANT bits6-10>0）
//     或已镶嵌（I_SOCKET bits0-3>0）的装备永不出售，在规则判定之前先行排除。

namespace autosell {

// 特殊类型位标志（多选；ItemView.special_types 与 Config.special_mask 按位匹配）。
enum SpecialType : uint32_t {
    kSpecialBackpack  = 1u << 0,  // 背包类
    kSpecialNormalSeal = 1u << 1,  // 普通徽章：勇士徽章 category 42..47
    kSpecialDice      = 1u << 2,  // 骰子
};

// 面板配置快照（与 sidecar `autosell` section 键一一对应；0 = 关闭，正整数为 1-based 档位）。
struct Config {
    bool enabled = false;  // 总开关；false 时恒不售

    // 装备规则（仅 is_equip 生效，三条之间为 AND：0=关不参与；已开启项须同时命中）。
    // 值 v：0=关；1..N → 出售对应量 <= v-1。
    int rarity = 0;   // 0=关；1..5 → 出售 rarity <= 值-1（1=白…5=紫，4 为最高品质）
    int enhance = 0;  // 0=关；1..32 → 出售 enhance_remaining(I_ENCHANT bits2-5) <= 值-1；UI 名称「强化耐久度」
    int socket = 0;   // 0=关；1..16 → 出售 socket_total(I_SOCKET bits4-7) <= 值-1

    // 宝石规则（仅 is_jewel 生效，同类内 OR）。
    int gem_tier = 0;  // 0=关；1..5 → 出售 jewel_tier(category-28) <= 值-1

    // 宝石属性范围规则（仅 is_jewel 生效，与 gem_tier 同类内 OR）。
    // 0=关；1..5 → 出售属性百分位 <= 阈值（1→30 / 2→60 / 3→75 / 4→90 / 5→100）。
    int gem_range = 0;

    // 特殊类型规则（对任意物品生效；0=关，非 0 位掩码与物品位标志按位与命中即出售）。
    uint32_t special_mask = 0;
};

// 物品视图（由上层从游戏内存/扩展袋读取后填充；本层不触碰游戏内存）。
struct ItemView {
    bool is_equip = false;
    int rarity = 0;         // 0..4
    // 强化筛选口径 = 剩余强化次数（用户 2026-09-15 裁决，UI 名称「强化耐久度」）；
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

// 判定单件物品是否应出售。纯函数，无副作用。
bool should_sell(const ItemView& item, const Config& cfg);

}  // namespace autosell
