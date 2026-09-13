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
