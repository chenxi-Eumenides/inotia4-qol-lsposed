#pragma once

#include <cstddef>
#include <cstdint>

#include "catalog.h"
#include "../../contracts/game_host.h"
#include "../../contracts/result.h"
#include "../../contracts/host_sdk.h"

// 自定义合成配方的 RECIPEBASE / MIXTUREBASE 表注入（custom-craft-recipe §4.3）。
//
// 纯构造器（build_record_bytes / derive_material_count / inject_into_buffers）不触碰游戏
// 内存，可 host 单测；custom_recipe_table_ensure() 为 Android 专属（读 .bss 指针全局、分配
// 模块自有缓冲、改写字段全局、绑定目录）。
namespace custom_recipe {

void set_context(const qol::contracts::QolGameHost* game,
                 const qol::contracts::QolHostSdk* sdk);

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

// 模块占用组位（目录派生 OR）；cat==nullptr / n==0 → 0。
uint8_t module_group_bits(const Def* cat, size_t n);

// 停用还原（纯逻辑）：把模块缓冲**原版段**（前 base_record_count 条）的 b11 逐记录还原为
// original_group_bits 快照值（注入前逐记录 b11，长度 >= base_record_count）。
// 目的：撤销 inject_into_buffers 对原版记录 b11 的组位清除。否则停用后模块页（如宝石强化
// group 3）的原版记录组位已被清掉，页面配方列表为空 → 标签页执行链空指针（真机 SIGSEGV）。
void restore_original_group_bits(uint8_t* buffer, uint16_t base_record_count, uint8_t recipe_size,
                                 const uint8_t* original_group_bits);

// 重新激活（纯逻辑）：按模块占用组位重新清除原版段 b11（与 inject_into_buffers 的清位一致），
// 保证注入态下原版记录不占用模块页（避免与原版记录重复出现在同一页）。
void clear_original_group_bits(uint8_t* buffer, uint16_t base_record_count, uint8_t recipe_size,
                               const uint8_t* original_group_bits, uint8_t module_bits);

// ---------------------------------------------------------------------------
// acquire / deactivate 的便携核心（Android 与 host 测试共用同一实现）
// ---------------------------------------------------------------------------
// Android 入口把真实 .bss 槽位装配成 TableAccess 后调用本核心；host 测试用可控内存装配
// 同一核心。分类、注入、分配、重入与 deactivate 语义因此只有一套实现，不存在第二份算法。

enum class TableAcquireStatus : uint8_t {
    kOk = 0,           // 注入完成，或对已注入表的合法重新激活
    kNotReady = 1,     // 尚未加载：base 未就绪 / 表指针缺失 / 尺寸或记录数为 0
    kUnsupported = 2,  // 永久不可用：非零但超上界的布局、目录不可用、书名册长度不变式不满足
    kInternal = 3,     // 内部错误（分配失败等）
    kBusy = 4,         // 重入：进行中的 acquire 未结束（不得因已有 buffer 报告成功）
};

// 与 Host 的约定：**只有 kNotReady 是「可自动恢复」信号**（宿主可在 SaveEnter 后做一次
// 定向 ownership 重试）；kBusy 本轮不作为自动恢复条件（重入只表示本次调用未开始），
// kUnsupported/kInternal 视为固定错误，不参与自动重试。

// 状态 → QolResult；调用方据此区分可重试（NotReady/Busy）与永久错误（Unsupported/Internal）。
QolResult table_status_result(TableAcquireStatus status);

using TableAllocFn = void* (*)(size_t);
using TableFreeFn = void (*)(void*);
using TableLogFn = void (*)(int32_t level, const char* message);

// 游戏侧表槽位：Android 指向可写 .bss 全局；host 测试指向任意内存。
struct TableAccess {
    uintptr_t base = 0;
    void** recipe_ptr_slot = nullptr;
    void** mixture_ptr_slot = nullptr;
    uint8_t* recipe_size_slot = nullptr;
    uint8_t* mixture_size_slot = nullptr;
    uint16_t* recipe_count_slot = nullptr;
};

// 模块侧状态：注入缓冲、原版段 b11 快照、base 缓存与重入标志。
// 不实现「已 Live 后游戏替换整张表」的监控：指针被改写时按既有语义只在下次 acquire 重新注入。
struct TableState {
    uint8_t* recipe_buffer = nullptr;
    uint8_t* mixture_buffer = nullptr;
    uint8_t* original_group_bits = nullptr;
    uint16_t original_group_count = 0;
    uint8_t original_group_recipe_size = 0;
    uint16_t base_record_count_cache = 0;
    bool busy = false;
};

// 「全局未就绪」日志去重状态（调用方可传 nullptr 表示不记录）。
struct TableLogDedupe {
    uint8_t last_recipe_size = 0xFF;
    uint8_t last_mixture_size = 0xFF;
    uint16_t last_count = 0xFFFF;
};

// 观测到的表状态（由 TableAccess 读出）。
struct TableProbe {
    bool base_ready = false;
    bool recipe_ptr_present = false;
    bool mixture_ptr_present = false;
    uint8_t recipe_size = 0;
    uint8_t mixture_size = 0;
    uint16_t record_count = 0;
    bool catalog_available = false;
};

// 分类：**先判非零非法上界**（避免「异常尺寸 + 另一字段为 0」被伪装成等待加载），
// 再判缺失字段，最后判目录可用性。
TableAcquireStatus classify_table_probe(const TableProbe& probe);

// 既有格式上界（与 table.cpp 原本的窄域校验一致）。
constexpr uint8_t kMaxRecipeRecordSize = 64;
constexpr uint8_t kMaxMixtureRecordSize = 16;
constexpr uint16_t kMaxTableRecordCount = 4096;

// acquire：busy → kBusy；否则按 classify_table_probe 分类并注入 / 合法重新激活。
// 成功时写回槽位与 state；失败不留下半注入状态（分配失败释放已分配部分）。
TableAcquireStatus table_acquire_core(const TableAccess& access, TableState* state,
                                      const Def* catalog, size_t catalog_len,
                                      TableAllocFn alloc, TableFreeFn release, TableLogFn log_fn,
                                      TableLogDedupe* dedupe);

// deactivate：**保留模块缓冲作为表体**（原表超集，越界读变界内读），只把 RECIPEBASE 记录数
// 改回原值，并在快照可用且记录尺寸一致时逐字节还原原版段组位。
// 指针已被游戏改写时只清模块状态，**绝不把（可能陈旧的）原表指针写回**。
TableAcquireStatus table_deactivate_core(const TableAccess& access, TableState* state);

// Android：读 .bss 槽位后调用核心；typed 结果供 ownership 路径区分错误域。
// - g_base==0 / 表指针缺失 / 尺寸或 count 为 0 → NotReady；
// - 非零非法上界、目录不可用、书名册不变式不满足 → Unsupported（永久）；
// - 分配失败 → Internal；重入 → Busy；其余 → OK。
QolResult custom_recipe_table_acquire();

// Android：`custom_recipe_table_acquire()` 的 bool 薄包装（保留给按 bool 判断的调用者）。
bool custom_recipe_table_ensure();

// Android：停用注入配方（见 table_deactivate_core 语义）。
void custom_recipe_table_deactivate();

}  // namespace custom_recipe
