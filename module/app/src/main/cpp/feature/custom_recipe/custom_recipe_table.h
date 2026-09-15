#pragma once

#include <cstddef>
#include <cstdint>

#include "feature/custom_recipe/custom_recipe_catalog.h"

// 自定义合成配方的 RECIPEBASE / MIXTUREBASE 表注入（custom-craft-recipe §4.3）。
//
// 纯构造器（build_record_bytes / derive_material_count / inject_into_buffers）不触碰游戏
// 内存，可 host 单测；custom_recipe_table_ensure() 为 Android 专属（读 .bss 指针全局、分配
// 模块自有缓冲、改写字段全局、绑定目录）。
namespace custom_recipe {

// 注入记录布局常量（RECIPEBASE 12B / MIXTUREBASE 3B；字段语义见设计书 §3.2）。
constexpr size_t kRecipeRecordSize = 12;
constexpr size_t kMixtureRecordSize = 3;

constexpr size_t kRbLabel = 0;          // b0-1 配方按钮文本 wordId（u16）
constexpr size_t kRbResultId = 2;       // b2-3 结果物品 id（u16，注入记录填 0）
constexpr size_t kRbMaterialStart = 4;  // b4-5 MIXTUREBASE 材料起始下标（u16）
constexpr size_t kRbMaterialCount = 6;  // b6 材料条目数（u8）
constexpr size_t kRbFlag7 = 7;          // b7（u8，原版恒 1）
constexpr size_t kRbCostWord = 8;       // b8-9 费用公式 wordId（u16）
constexpr size_t kRbUnlockGate = 10;    // b10 解锁门槛（u8，注入记录必须为 0，§7.8）
constexpr size_t kRbGroup = 11;         // b11 组位图（u8，bit g = group g）

constexpr uint8_t kRbFlag7Value = 1;
constexpr uint8_t kRbUnlockGateValue = 0;
constexpr uint8_t kRbGroupBitCount = 8;  // b11 位宽（Def::group 须 < 8；越界视为非法，注入记录不占任何组）

// 注入记录 b11 的组位：`1 << Def::group`（bit0=0 免装备校验：CheckMixture 直接返回 0、
// MakeItem 走通用路径由 hook 拦截；bit5=0 非配方书）。group >= 8 返回 0（该记录不出现在任何页）。
constexpr uint8_t recipe_group_bit(uint8_t group) {
    return group < kRbGroupBitCount ? static_cast<uint8_t>(1u << group) : 0u;
}

// 按目录派生一条注入用 RECIPEBASE 记录（12 字节，小端）。
void build_record_bytes(const Def& def, uint16_t material_start, uint8_t out[kRecipeRecordSize]);

// 遍历原版 RECIPEBASE 求 max(b4-5 + b6)（原版 = 189）；record_count==0 → 0。
uint32_t derive_material_count(const uint8_t* recipe, uint16_t record_count, uint8_t record_size);

// 纯注入：把原表复制进 out_recipe / out_mixture（复制后清掉原版记录上模块占用的组位，只动
// b11），尾部追加各配方记录与材料条目。
// out_recipe 容量 >= (base_record_count + n) * recipe_size；
// out_mixture 容量 >= (derived_material_count + material_total) * mixture_size。
// 返回注入后 RECIPEBASE 记录数（= base_record_count + n）。
uint32_t inject_into_buffers(const uint8_t* orig_recipe, uint16_t base_record_count,
                             uint8_t recipe_size, const uint8_t* orig_mixture,
                             uint8_t mixture_size, const Def* cat, size_t n,
                             uint8_t* out_recipe, uint8_t* out_mixture);

// Android：校验并（重新）注入；幂等、可重复调用。
// - 已注入且游戏侧指针仍指向模块缓冲 → 只把记录数归位到 base + N 并返回 true；
// - 游戏重新装载静态表导致指针被改写 → 用新原表重新注入；
// - bridge 未就绪 / 表指针非法 → 返回 false（fail-closed，绝不带着垃圾指针 memcpy）。
bool custom_recipe_table_ensure();

// Android：停用注入配方。**保留模块缓冲作为表体**（它是原表的超集，越界读因此变成界内读），
// 只把 RECIPEBASE 记录数改回原值，使任何配方查询都不会再命中注入记录。
// 这样面板残留的配方数组即便仍持有注入下标，绘制读也在缓冲界内，不会越界。
// 仅在注入仍生效（指针仍指向模块缓冲）时执行；下次 ensure() 会把记录数归位回来。
void custom_recipe_table_deactivate();

}  // namespace custom_recipe
