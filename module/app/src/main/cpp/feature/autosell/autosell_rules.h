#pragma once

#include <cstdint>

// 自动出售纯规则引擎：只做「给定物品视图 + 配置 -> 是否出售」的判定。
// 零游戏/Android 依赖（纯 STL，编入 host 单测）；扫描、处置与 UI 由上层负责。
//
// 语义依据 docs/development/features/auto-sell.md §4.3、§9（用户 2026-09-12/13 裁决）：
//   - 总开关关闭恒不出售。
//   - 同类内 OR：装备满足任一已启用装备规则即命中；宝石满足任一已启用宝石规则即命中。
//   - 跨类独立：装备规则只对 is_equip 生效；宝石规则只对 is_jewel 生效；特殊类型对任意物品生效。
//   - 比较方向统一 `<= 所选档`。
//   - 任一维度命中即出售（全局 OR）；未启用任何规则时不得出售。

namespace autosell {

// 特殊类型位标志（多选；ItemView.special_types 与 Config.special_mask 按位匹配）。
enum SpecialType : uint32_t {
    kSpecialBackpack      = 1u << 0,  // 背包类
    kSpecialMercenarySeal = 1u << 1,  // 英雄徽章
    kSpecialEnchantScroll = 1u << 2,  // 强化卷轴
    kSpecialDice          = 1u << 3,  // 骰子
    kSpecialSealed        = 1u << 4,  // 可解封
    kSpecialItemBox       = 1u << 5,  // 开箱
};

// 面板配置快照（与 ModuleConfig 键一一对应；阈值语义见各自注释）。
struct Config {
    bool enabled = false;  // 总开关；false 时恒不售

    // 装备规则（仅 is_equip 生效，同类内 OR）
    bool rarity_enabled = false;
    int rarity_threshold = 0;   // 0..4，出售 rarity <= 阈值
    bool enhance_enabled = false;
    int enhance_threshold = 0;  // 总强化次数（I_ENCHANT bits6-10），出售 <= 阈值
    bool socket_enabled = false;
    int socket_threshold = 0;   // 总孔数（I_SOCKET bits4-7），出售 <= 阈值

    // 宝石规则（仅 is_jewel 生效，同类内 OR）
    bool gem_tier_enabled = false;
    int gem_tier_threshold = 0;  // 0..4（category-28），出售 jewel_tier <= 阈值

    // 特殊类型规则（对任意物品生效；mask 与物品位标志按位与命中即出售）
    bool special_enabled = false;
    uint32_t special_mask = 0;
};

// 物品视图（由上层从游戏内存/扩展袋读取后填充；本层不触碰游戏内存）。
struct ItemView {
    bool is_equip = false;
    int rarity = 0;         // 0..4
    int enhance_count = 0;  // 总强化次数 = (I_ENCHANT >> 6) & 0x1F
    int socket_total = 0;   // 总孔数 = (I_SOCKET >> 4) & 0x0F
    bool is_jewel = false;
    int jewel_tier = 0;  // 0..4（category 28..32）
    uint32_t special_types = 0;
};

// 判定单件物品是否应出售。纯函数，无副作用。
bool should_sell(const ItemView& item, const Config& cfg);

}  // namespace autosell
