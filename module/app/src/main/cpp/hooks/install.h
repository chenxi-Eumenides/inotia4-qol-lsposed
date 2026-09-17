#pragma once

// 入口 hook 安装引擎（ARCHITECTURE.md §4.1 / §4.3 声明式安装）。
//
// 本 TU 只做「按声明调用宿主注入的 hook 原语并落状态」，物理还原（unhook）与
// 帧步进事务属 P3，本 TU 不提供。
//
// 安装原语由宿主注入（NativeHookFn），本模块不直接调用 LSPosed API：宿主可测，
// 且 check_resources.py 不会把机制本体误报为资源安装点。

#include <cstdint>

#include "contracts/plugin.h"

namespace qol::hooks {

// 宿主注入的 hook 原语：target 为解析后的入口地址，replacement 为 claim.handler，
// 成功返回 0 并把 backup 写出。
using NativeHookFn = int (*)(void* target, void* replacement, void** backup);

// ok 非 0 = 失败；error 为静态字符串（成功时为 nullptr）。
struct InstallOutcome {
    int32_t ok;
    const char* error;
    void* backup;
};

// 声明式安装一个入口 hook 资源：
// - claim 未登记 → "claim not registered"；
// - mechanism != kEntryHook → "mechanism mismatch"；
// - handler == nullptr → "handler missing"；
// - resolved_entry == 0 或 hook_fn == nullptr → "not ready"；
// - hook_fn 非 0 → "native hook failed"（不改登记表状态）；
// - 成功 → 状态置 gate_on ? kLive : kDisabled，返回 backup。
InstallOutcome install_claim_entry_hook(const contracts::ResourceClaim& claim,
                                        uintptr_t resolved_entry,
                                        NativeHookFn hook_fn,
                                        bool gate_on);

}  // namespace qol::hooks
