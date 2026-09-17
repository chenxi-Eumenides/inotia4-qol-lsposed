#include "hooks/install.h"

// 入口 hook 安装引擎实现（见 hooks/install.h 的接口契约）。

#include "hooks/registry.h"

namespace qol::hooks {
namespace {

InstallOutcome failure(const char* error) {
    return InstallOutcome{1, error, nullptr};
}

ReleaseOutcome release_failure(const char* error) {
    return ReleaseOutcome{1, error};
}

}  // namespace

InstallOutcome install_claim_entry_hook(const contracts::ResourceClaim& claim,
                                        uintptr_t resolved_entry,
                                        NativeHookFn hook_fn,
                                        bool gate_on) {
    if (find_claim(claim.id) == nullptr) {
        return failure("claim not registered");
    }
    if (claim.mechanism != contracts::ResourceMechanism::kEntryHook) {
        return failure("mechanism mismatch");
    }
    if (claim.handler == nullptr) {
        return failure("handler missing");
    }
    if (resolved_entry == 0 || hook_fn == nullptr) {
        return failure("not ready");
    }

    void* backup = nullptr;
    // 机制本体由宿主注入；本层只按声明传参并解释返回值。
    const int rc = hook_fn(reinterpret_cast<void*>(resolved_entry), claim.handler, &backup);
    if (rc != 0) {
        // 安装失败不改登记表：占用状态保持安装前的值，交上层按 §8 决定回滚/降级。
        return failure("native hook failed");
    }

    set_state(claim.id, gate_on ? contracts::ResourceState::kLive
                                : contracts::ResourceState::kDisabled);
    return InstallOutcome{0, nullptr, backup};
}

ReleaseOutcome release_claim_entry_hook(const contracts::ResourceClaim& claim,
                                        uintptr_t resolved_entry,
                                        NativeUnhookFn unhook_fn) {
    if (find_claim(claim.id) == nullptr) {
        return release_failure("claim not registered");
    }
    if (claim.mechanism != contracts::ResourceMechanism::kEntryHook) {
        return release_failure("mechanism mismatch");
    }
    if (resolved_entry == 0 || unhook_fn == nullptr) {
        return release_failure("not ready");
    }

    // unhook 原语以原函数入口为句柄（LSPosed unhook 语义）；失败不改登记表。
    const int rc = unhook_fn(reinterpret_cast<void*>(resolved_entry));
    if (rc != 0) {
        return release_failure("native unhook failed");
    }

    // 物理归还成功：占用回到 free，运行期记录一并清除。
    set_state(claim.id, contracts::ResourceState::kFree);
    runtime_clear(claim.id);
    return ReleaseOutcome{0, nullptr};
}

}  // namespace qol::hooks
