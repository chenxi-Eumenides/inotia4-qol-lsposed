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

// b11 bit5 = 「配方书条目」（传说装备页成员）。**注入记录必须同时置这一位**，原因：
//   `MIXSYSTEM_AddRecipeBook(idx)` 的位索引 = `idx + GetRecipeCount(5) - GetRecipeCount(0)`；
//   `MIXSYSTEM_MakeRecipeList(5)` 的逆映射 = `GetRecipeCount(0) - GetRecipeCount(5) + 位`。
//   其中 **`GetRecipeCount(0)` 不是统计 bit0，而是直接返回 RECIPEBASE 总记录数**（group==0 是特殊
//   分支：`GetRecipeCount@0x11b3a8` 命中 → `0x11b42c` 读 `0x3019ba` 的记录数全局）。原版
//   69 - 52 = 17 = 第一条 bit5 记录的索引，两侧互为逆运算，自洽。
//   本模块把总记录数改成 69+N；若 N 条注入记录不计入 bit5，则 base 变成 17+N —— 既有解锁在传说
//   装备页整体错位 N 条，且 `idx < 17+N` 的记录会解算出**负位索引**；`AddRecipeBook` 只校验上界
//   （`asr w1,w19,#3; cmp w1,w0; b.ge`）不校验下界 → 对书名册缓冲**前方越界读改写**。
//   置位后 base = (69+N) - (52+N) = 17 对任意 N 恒成立。
constexpr uint8_t kRbRecipeBookBit = 0x20;

// 注入记录 b11 的组位：`1 << Def::group`（bit0=0 免装备校验：CheckMixture 直接返回 0、
// MakeItem 走通用路径由 hook 拦截）。group >= 8 返回 0（该记录不出现在任何页；仍须由
// build_record_bytes 补上 kRbRecipeBookBit，否则上面的计数不变式被破坏）。
constexpr uint8_t recipe_group_bit(uint8_t group) {
    return group < kRbGroupBitCount ? static_cast<uint8_t>(1u << group) : 0u;
}

// 统计一条 RECIPEBASE 中 bit5（配方书条目）的记录数。recipe==nullptr / record_size <= kRbGroup → 0。
uint32_t count_recipe_book_records(const uint8_t* recipe, uint16_t record_count,
                                   uint8_t record_size);

// 书名册（MIXSYSTEM_pRecipeBook@0x307760）的字节数，游戏侧等价于 `MIXSYSTEM_GetRecipeBookSize()`
// = `(GetRecipeCount(5) + 7) / 8`（0x11b450）。
// ⚠️ 注入 N 条后 count5 变为 count5+N，该值**必须与注入前相等**：
//   - 书名册缓冲由游戏在启动时按注入前的大小分配，变大即越界读写；
//   - 存档只读写配方书位图、长度按 `GetRecipeCount(5)` 动态（§7.10），变大同时改变存档格式长度。
// 以原版 52 为例：52..56 都是 7 字节，57 起变 8 字节 → 注入条数上限受此约束（当前 N=2 安全）。
constexpr uint32_t recipe_book_bytes(uint32_t recipe_book_count) {
    return (recipe_book_count + 7u) / 8u;
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
