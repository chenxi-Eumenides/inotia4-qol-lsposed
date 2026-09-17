#pragma once

#include <cstdint>

// 行为所有权契约（ARCHITECTURE.md §4.1 kOwnership）：非 hook/patch 的互斥槽占有
// （如共享表注入、跨插件行为接管）。资源声明 mechanism=kOwnership 时，claim.handler
// 指向本结构（静态存储，生命周期 ≥ 进程）；宿主 install/release 非入口路径经本结构
// 分派，不再假设调用点 patch 站点。
//
// 契约类型限制同 contracts/plugin.h：POD + 固定宽度 C 类型 + 函数指针。

namespace qol::contracts {

struct OwnershipOps {
    // 取得所有权；0 = 成功，非 0 = 失败（错误码）。
    int32_t (*acquire)(const char* id);
    // 释放所有权（幂等清理）；可为 nullptr（无可释放动作）。
    void (*release)(const char* id);
};

// claim.handler → OwnershipOps*；handler 为空返回 nullptr。
inline const OwnershipOps* ownership_ops(void* handler) {
    return static_cast<const OwnershipOps*>(handler);
}

// 执行 acquire；ops/acquire 缺失或返回非 0 均视为失败（返回非 0）。
inline int32_t ownership_acquire(const OwnershipOps* ops, const char* id) {
    if (ops == nullptr || ops->acquire == nullptr) return -1;
    return ops->acquire(id);
}

// 执行 release（幂等）；ops/release 缺失时静默。
inline void ownership_release(const OwnershipOps* ops, const char* id) {
    if (ops != nullptr && ops->release != nullptr) {
        ops->release(id);
    }
}

}  // namespace qol::contracts
