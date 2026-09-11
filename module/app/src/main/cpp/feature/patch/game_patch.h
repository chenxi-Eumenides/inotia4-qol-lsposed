#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// 游戏内存 patch 基础设施：S2 数量布局（a=bits22-24、b=bits25-31、count=128a+b）
// 下的常驻位段 patch，以及可逆的 99/999 上限 clamp 指令。所有地址走 fn_resolve 符号
// 解析，禁止裸地址。

struct PatchEntry {
    const char* func_macro;   // 函数符号宏名（symbol_registry.h 的 SYM 宏名，fn_resolve 用）
    uintptr_t func_vma;       // 函数 VMA（动态解析失败时回退）
    uint32_t func_offset;     // 函数内偏移
    uint32_t orig;            // 原始指令（关闭时还原）
    uint32_t replacement;     // 替换指令
};

// 常驻位段 patch：S2 下原版 b 字段（bits25-31）读写天然正确，仅保留存档子物品
// 检查的位段缩窄（避开数量高位段 a）；装备/损坏 marker、袋容量、宝石选项等
// 非数量位段不得加入此表。
extern const PatchEntry g_stack_layout_patches[];
extern const size_t g_stack_layout_patch_count;

// 配置开关只控制完整数量判定点的 99/999 clamp，不得卸载常驻位段 patch；
// 直接作用于 b 段残量的判定点不得进此表（见 game_patch_core.inc 的口径注释）。
extern const PatchEntry g_stack_limit_clamp_patches[];
extern const size_t g_stack_limit_clamp_patch_count;

// ---- S2 直接位读重定向（VM-37）----
// 出售/拆堆路径原以 `ldr w0,[xN,#0x10]; bl UTIL_GetBitValue(_,31,25)` 只读 b 段，
// S2 下整堆出售判定、出售数量输入框上限与拆堆守卫都按 b 残量（真机实证：200 个
// 中药水按 b=72 结算）。重定向为 `mov x0,xN; bl ITEM_GetCumulateCount(item)`，
// 经统一 getter（类别门控 fail-closed + 模式感知解码，R-46/R-47/R-49）读全量。
// getter 关闭态返回 b，与原版逐位一致，因此重定向常驻、不随上限开关 revert。
// 注意：不可把 GetBitValue 的 start 常量 25→22 当全量读——十位窗口 (v>>22)&0x3FF
// 在数值上是 `a + 8b`（a 在窗口低位、b 在高位），与 `a*128+b` 不等价。
struct S2ReadRedirectEntry {
    const char* func_macro;    // 调用点所在函数符号宏名（symbol_registry.h 的 SYM 宏名）
    uintptr_t func_vma;        // 调用点函数 VMA（动态解析失败时回退）
    uint32_t arg_offset;       // 物品指针来源指令偏移（ldr w0,[xN,#0x10] 或 mov w0,wN）
    uint32_t arg_orig;         // 原始指令
    uint32_t arg_replacement;  // mov x0, xN（把物品指针而非字段值交给 getter）
    uint32_t call_offset;      // bl UTIL_GetBitValue 偏移
    uint32_t call_orig;        // 原始 bl（同 .so 内目标，replacement 运行时按 g_base 计算）
    const char* target_macro;  // 目标函数符号宏名
    uintptr_t target_vma;      // 目标函数 VMA
};
extern const S2ReadRedirectEntry g_stack_getter_redirect_patches[];
extern const size_t g_stack_getter_redirect_patch_count;

// 重定向常驻 patch 的 apply/revert（apply_fixed_stack_layout 内 apply 一次）。
bool apply_s2_getter_redirects();
bool revert_s2_getter_redirects();

bool patch_apply(const PatchEntry* entries, size_t n);
bool patch_revert(const PatchEntry* entries, size_t n);

class PatchSet {
public:
    PatchSet(const PatchEntry* entries, size_t count) : entries_(entries), count_(count) {}

    bool apply() {
        if (entries_ == nullptr || !patch_apply(entries_, count_)) return false;
        applied_ = true;
        return true;
    }

    bool revert() {
        if (entries_ == nullptr || !patch_revert(entries_, count_)) return false;
        applied_ = false;
        return true;
    }

    bool applied() const { return applied_; }

private:
    const PatchEntry* entries_ = nullptr;
    size_t count_ = 0;
    bool applied_ = false;
};

bool apply_fixed_stack_layout();

bool set_stack_limit_enabled(bool enabled);
bool stack_limit_enabled();

// ---- UIEquip 背包格事件 wrapper（v0.6.8） ----
// wrapper 始终保留扩展源保护；只有原版同类合并行为由 moveMergeEnabled 控制。
bool set_move_merge_enabled(bool enabled);
bool move_merge_enabled();
bool set_extension_source_protection_enabled(bool enabled);

// ---- IAP 恢复 + 批量宝石合成按钮 ----
void data_op_mix_gem_batch(void* ctrl);
bool data_craft_btn_inject();
void data_craft_btn_remove();
void data_craft_btn_set_enabled(bool enabled);

// ---- IAP 恢复（v0.5.18 hive 屏蔽恢复）----
std::string data_recover_after_hive_block();
