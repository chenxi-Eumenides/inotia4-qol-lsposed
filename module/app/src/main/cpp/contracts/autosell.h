#pragma once

#include <cstdint>

// 自动出售纯类型契约。

namespace autosell {

// 面板配置快照（与 sidecar `autosell` section 键一一对应；0 = 关闭，正整数为 1-based 档位）。
struct Config {
    bool enabled = false;  // 总开关；false 时恒不售

    // 装备规则（仅 is_equip 生效，三条之间为 AND：0=关不参与；已开启项须同时命中）。
    // 值 v：0=关；1..N → 出售对应量 <= v-1。
    int rarity = 0;   // 0=关；1..5 → 出售 rarity <= 值-1（1=白…5=紫，4 为最高品质）
    int enhance = 0;  // 0=关；1..32 → 出售 enhance_remaining(I_ENCHANT bits2-5) <= 值-1
    int socket = 0;   // 0=关；1..16 → 出售 socket_total(I_SOCKET bits4-7) <= 值-1

    // 宝石规则（仅 is_jewel 生效，同类内 OR）。
    int gem_tier = 0;  // 0=关；1..5 → 出售 jewel_tier(category-28) <= 值-1

    // 宝石属性规则（仅 is_jewel 生效，与 gem_tier 同类内 OR）。
    // 0=关；1..5 → 出售属性百分位 <= 阈值（1→30 / 2→60 / 3→75 / 4→90 / 5→99）。
    int gem_range = 0;

    int backpack_tier = 0;  // 0=关；1..4 → <=手提包/背包小/背包中/背包大
    int badge_tier = 0;     // 0=关；1..4 → <=普通/被祝福/教团/英雄
    int dice_tier = 0;      // 0=关；1..5 → <=低级/中级/高级/顶级/混沌
};

}  // namespace autosell
