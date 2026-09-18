#pragma once

#include <cstdint>

// 通用 BL 调用点 patch（自动出售阶段 A 基础设施）。
//
// 语义：把 call_addr 处的原指令改写为 `bl <wrapper>`；当 wrapper 不在
// call_addr 的 ±128MB BL 可达范围内时，先在近址分配一段 thunk（thunk 内
// `ldr x16, wrapper; blr x16; ret`）再改指 thunk。
//
// fail-closed：安装前必须校验 call_addr 当前指令字等于 expected_word，不符即
// 返回 false 且不改写任何字节；revert 只在当前指令确为 BL 时恢复，否则返回
// false。install/revert 与 thunk 页分配共享内部互斥锁，线程安全。
bool call_patch_install_bl(uintptr_t call_addr, uint32_t expected_word, void* wrapper);

// 恢复 call_addr 处原始指令字 original_word。
bool call_patch_revert_bl(uintptr_t call_addr, uint32_t original_word);

// 通用指令字替换（如条件分支→NOP）。fail-closed：当前指令字 != expected_word 时
// 不改写并返回 false；当前指令字 == new_word 视为已安装，幂等返回 true。
bool call_patch_install_word(uintptr_t addr, uint32_t expected_word, uint32_t new_word);

// B 尾调用重定向（kBranchRedirect）：站点为 B 指令；在 call_addr ±128MB 内复用 thunk 池
// 分配 thunk（`ldr x16, #8; br x16; .quad handler`）并把 B 改指 thunk；返回 thunk 运行期
// 地址，0 = 失败（超范围分配不到 / 原字不符 / 地址非法；均不改写任何字节）。B 不改写 x30，
// thunk 以 `br` 进入 handler，handler 的 `ret` 直接返回原调用者，等价原尾调用。
// 幂等：同一站点 + 同一 handler 重复调用返回既有 thunk。
uintptr_t call_patch_install_branch(uintptr_t call_addr, uint32_t expected_word, void* handler);

// 还原 B 重定向：写回原字 original_word；当前已是原字时幂等返回 true。
// 当前指令非 B 时 fail-closed 返回 false（不覆盖非 B 指令）。
bool call_patch_revert_branch(uintptr_t call_addr, uint32_t original_word);

// 恢复 addr 处原始指令字 original_word；当前指令字已是 original_word 时幂等返回 true。
bool call_patch_revert_word(uintptr_t addr, uint32_t original_word);
