#pragma once

// 4 字节短跳入口 hook。
//
// 动机：LSPosed 注入的 hook 原语（Dobby）在目标与替换入口超出 ±128MB 时写
// `adrp/add/br`（12 字节），把入口之后的指令一并覆盖。monster 改版把若干函数入口
// 改写成 `b <隐藏段桩>`，桩返回时会跳回入口 +8 处，被覆盖后即执行补丁字面量导致跑飞。
//
// 本原语只改写入口第一条指令：写 `b <近址 thunk>`，thunk 内绝对跳转到 wrapper；
// 被顶掉的那条指令复制到近址 trampoline，再从 trampoline 跳回入口+4。trampoline 同时
// 作为 wrapper 的 backup（原函数入口）。
//
// 支持的首条指令：无条件 B、CBZ/CBNZ、TBZ/TBNZ、以及所有非 PC 相对指令。
// 其余 PC 相对形式（BL、B.cond、ADR/ADRP、LDR literal）一律 fail-closed，不改写任何字节。

#include <cstdint>

namespace qol::patch {

struct ShortHookResult {
    int32_t ok;          // 0 = 成功
    const char* error;   // 静态字符串，成功为 nullptr
    void* backup;        // trampoline：供 wrapper 调用原函数
};

// entry 必须是 4 字节对齐、可执行且当前指令可读。
ShortHookResult short_hook_install(uintptr_t entry, void* handler);

// 还原 entry 处原始指令并归还近址槽；未安装时返回 false。
bool short_hook_remove(uintptr_t entry);

// entry 是否由本原语安装。
bool short_hook_contains(uintptr_t entry);

// 供宿主装配 hooks::ShortHookOps 使用的适配器（hooks/ 不依赖 patch/）。
int32_t short_hook_install_adapter(uintptr_t entry, void* handler, void** backup);

}  // namespace qol::patch
