#pragma once

#include <cstdint>
#include "host_sdk.h"
#include "owner_token.h"

namespace qol::contracts {
enum class QolSlotOperationApplyState : uint32_t {
    kUnchanged = 0, kPrimaryApplied = 1, kFullyApplied = 2,
};
// 一次多槽操作的准入凭据：acquire 原子取得整个 slot_mask（bit0..2，非零），
// 不能按槽拆开或部分占用。id 进程内不复用；referenced 在首次不可逆变更前置位，
// 置位后 owner 停用不释放；referenced 实际变更时递增 revision，旧副本凭据拒绝。
struct QolSlotOperationLease {
    uint32_t size;
    uint32_t slot_mask;
    uint64_t id;
    QolOwnerToken owner;
    uint64_t revision;
    uint32_t referenced;
    uint32_t reserved;
};
struct QolSlotOperationApply {
    uint32_t size;
    uint32_t apply; // QolSlotOperationApplyState；失败路径保持已到达的最远状态。
};
// 中立的多槽操作准入，不包含健康状态、journal 或游戏保存语义。
struct QolSlotOperationApi {
    QolAbiHeader abi;
    void* host_ctx;
    QolResult (*acquire)(void*, QolOwnerToken, uint32_t, QolSlotOperationLease*);
    QolResult (*set_referenced)(void*, QolOwnerToken, QolSlotOperationLease*, uint32_t);
    QolResult (*refresh)(void*, QolOwnerToken, uint64_t, QolSlotOperationLease*);
    QolResult (*release)(void*, QolOwnerToken, const QolSlotOperationLease*);
    QolResult (*import_container)(void*, QolOwnerToken, const QolSlotOperationLease*, int32_t,
                                 const uint8_t*, uint64_t, QolSlotOperationApply*);
    QolResult (*delete_container)(void*, QolOwnerToken, const QolSlotOperationLease*, int32_t);
};
}  // namespace qol::contracts
