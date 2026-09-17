#pragma once

#include <cstdint>

// 游戏层：可安装入口解析（ARCHITECTURE.md §5「版本耦合唯一收口」）。
// 符号名登记、VMA 兜底与 monster 跳板跟随细节留在本层；上层只按声明符号名取入口，
// 不接触 VMA/偏移。

namespace qol::game {

// 按声明的符号名解析可安装入口；未登记返回 0。
// monster 变体把入口改写成无条件 b 跳板时主动跟随一次（理由见 .cpp 注释）。
// out_followed 非空时写出「是否跟随了跳板」。
uintptr_t resolve_hook_entry(const char* symbol_name, bool* out_followed);

}  // namespace qol::game
