#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// 游戏内存 patch 基础设施：固定 canonical 数量位段 bit22-31，
// 以及可逆的 99/999 上限 clamp 指令。所有地址走 fn_resolve 符号解析，禁止裸地址。

struct PatchEntry {
    const char* func_macro;   // 函数符号宏名（symbol_registry.h 的 SYM 宏名，fn_resolve 用）
    uintptr_t func_vma;       // 函数 VMA（动态解析失败时回退）
    uint32_t func_offset;     // 函数内偏移
    uint32_t orig;            // 原始指令（关闭时还原）
    uint32_t replacement;     // 替换指令
};

// 固定布局 patch：位段扩展 31 + 存档子物品检查 2。
extern const PatchEntry g_stack_layout_patches[];
extern const size_t g_stack_layout_patch_count;

// 配置开关只控制 99/999 clamp，不得卸载固定布局。
extern const PatchEntry g_stack_limit_clamp_patches[];
extern const size_t g_stack_limit_clamp_patch_count;

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

// ---- move-merge（v0.6.8）：游戏内拖拽移动物品触发同类合并；由 moveMergeEnabled 配置控制 ----
// 覆盖 UIEquip 背包格控件事件处理器（GOT 0x2f5410）：
// 拖拽移动（event==4）时若源/目标同类且可堆叠，不清空目标槽直接调 INVEN_MoveItem 合并
//（数量并入目标槽、超出留源槽），其余场景回退原交换/移动逻辑。
bool set_move_merge_enabled(bool enabled);
bool move_merge_enabled();

// ---- IAP 恢复 + 批量宝石合成按钮 ----
void data_op_mix_gem_batch(void* ctrl);
bool data_craft_btn_inject();
void data_craft_btn_remove();
void data_craft_btn_set_enabled(bool enabled);

// ---- IAP 恢复（v0.5.18 hive 屏蔽恢复）----
std::string data_recover_after_hive_block();
