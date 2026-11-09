#pragma once

#include <cstdint>

// 游戏层：可安装入口解析（ARCHITECTURE.md §5「版本耦合唯一收口」）。
// 符号名登记、VMA 兜底与 monster 跳板跟随细节留在本层；上层只按声明符号名取入口，
// 不接触 VMA/偏移。

namespace qol::game {

constexpr const char* kElfExportPrefix = "elf-export:";

// Returns nullptr for the legacy symbol-ID namespace; returns the suffix for a
// valid explicit ELF export claim, including an empty suffix for invalid input.
inline const char* elf_export_name(const char* symbol_name) {
    constexpr uint32_t kPrefixLength = 11;
    if (symbol_name == nullptr) return nullptr;
    for (uint32_t i = 0; i < kPrefixLength; ++i) {
        if (symbol_name[i] == '\0' || symbol_name[i] != kElfExportPrefix[i]) return nullptr;
    }
    return symbol_name + kPrefixLength;
}

// 按声明的符号名解析可安装入口；未登记返回 0。
// monster 变体把入口改写成无条件 b 跳板时只上报 out_followed，不跟随（安装层改用 4 字节短补丁）。
// out_followed 非空时写出「入口是否为 b 跳板」。
uintptr_t resolve_hook_entry(const char* symbol_name, bool* out_followed);

// opt-in 跟随：入口是 b 跳板且目标可安全 hook 时返回跳板目标，否则回退到 resolve_hook_entry 的入口。
// 用于「入口被外部周期性还原、但跳板目标稳定」的落点（见 docs/refactor/hook-engine-decision.md）。
// 目标首条是短桩类控制流（b/bl/b.cond/cbz/tbz/br/blr/ret）时拒绝跟随，避免 Dobby 12/16 字节
// 补丁覆盖「桩返回入口+8」的落点。
uintptr_t resolve_hook_entry_follow(const char* symbol_name, bool* out_followed);

}  // namespace qol::game
