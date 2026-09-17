#pragma once

#include <cstdint>

// 自动出售稳定端口（ARCHITECTURE.md §3.4 契约/端口风格；与 contracts/attribute_range.h、
// core/native/*_port.h 同类）。只放消费者（UI/JNI）需要的纯类型与函数声明，实现由
// plugins/autosell 提供。UI 经本头访问，避免 plugins/ui → plugins/autosell 的
// feature→feature include（属 P4-B3 迁移的正常收口）。

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
    int enhance = 0;  // 0=关；1..32 → 出售 enhance_remaining(I_ENCHANT bits2-5) <= 值-1
    int socket = 0;   // 0=关；1..16 → 出售 socket_total(I_SOCKET bits4-7) <= 值-1

    // 宝石规则（仅 is_jewel 生效，同类内 OR）。
    int gem_tier = 0;  // 0=关；1..5 → 出售 jewel_tier(category-28) <= 值-1

    // 宝石属性范围规则（仅 is_jewel 生效，与 gem_tier 同类内 OR）。
    // 0=关；1..5 → 出售属性百分位 <= 阈值（1→30 / 2→60 / 3→75 / 4→90 / 5→100）。
    int gem_range = 0;

    // 特殊类型规则（对任意物品生效；0=关，非 0 位掩码与物品位标志按位与命中即出售）。
    uint32_t special_mask = 0;
};

}  // namespace autosell

// 运行时配置与持久化接口：声明在全局命名空间（实现见 plugins/autosell），
// 与 plugins/autosell 内的定义同名同签名；消费者（UI/JNI）经本头调用。
autosell::Config autosell_get_runtime_config();
void autosell_apply_config(const autosell::Config& config);
bool autosell_store_persist(int slot, const autosell::Config& config);
