#pragma once

#include <cstdint>

#include "owner_identity.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kQolSlotDataAdminCapabilityId = "save.slot_admin";
constexpr QolVersion kQolSlotDataAdminVersion{1, 0};
using QolSlotAdminToken = uint64_t;
using QolSlotAdminOperation = uint64_t;
using QolSlotGeneration = uint64_t;
constexpr QolSlotAdminToken kQolSlotAdminTokenInvalid = 0;
constexpr QolSlotAdminOperation kQolSlotAdminOperationInvalid = 0;

enum class QolSlotAdminOp : uint32_t {
    kExport = 0, kImport = 1, kDelete = 2, kRollback = 3,
};
// 导入可能 primary 成功、last-good 失败而返回 IO error（infra/store.cpp），
// 不能仅用 bool 判断是否需要回滚。
enum class QolSlotAdminApply : uint32_t {
    kUnchanged = 0, kPrimaryApplied = 1, kFullyApplied = 2,
};
// generation 仅在完整备份操作成功或完整回滚结束后递增。
enum class QolSlotAdminPhase : uint32_t {
    kPreDat = 0, kPostDat = 1,
};
struct QolSlotAdminResult {
    uint32_t size;
    uint32_t op;     // QolSlotAdminOp
    uint32_t apply;  // QolSlotAdminApply
    uint32_t reserved;
    int32_t slot;
    uint64_t generation_after;
    uint64_t bytes;  // read_container 返回长度，其他操作为 0。
    QolResult result;
};

// 容器级管理不是普通参与者接口。仅授予宿主装配明确授权的 owner；
// core.save 内部直接管理建档/reset，不走此面，不按插件名称特判授权。
using QolSlotAdminOpenFn = QolResult (*)(QolOwnerToken owner, QolSlotAdminToken* out_admin);
// pending 保存事务存在则拒绝。导入当前已加载槽默认拒绝，要求先退主菜单，
// 防止运行时旧状态在下一次保存覆盖导入内容。
// 操作令牌跨完整 dat/sidecar/warehouse 事务持有，但不跨调用持有 mutex。
using QolSlotAdminBeginFn = QolResult (*)(QolSlotAdminToken admin, int32_t slot,
    uint32_t op, uint64_t expected_generation, QolSlotAdminOperation* out_operation);
using QolSlotAdminReadContainerFn = QolResult (*)(QolSlotAdminOperation operation,
    uint8_t* out_buffer, uint64_t capacity, uint64_t* out_size);
// 未知功能 section 原样保留；导入的未决 journal 不盲目重放。
using QolSlotAdminImportContainerFn = QolResult (*)(QolSlotAdminOperation operation,
    const uint8_t* bytes, uint64_t size);
using QolSlotAdminDeleteContainerFn = QolResult (*)(QolSlotAdminOperation operation);
using QolSlotAdminRestoreContainerFn = QolResult (*)(QolSlotAdminOperation operation,
    const uint8_t* snapshot, uint64_t size);
// 仅完整备份操作成功或完整回滚结束后调用，不能 sidecar 写完即提前调用。
// committed=0 表示完整回滚结束；两者均递增 generation 并失效运行时 epoch。
using QolSlotAdminEndFn = QolResult (*)(QolSlotAdminOperation operation,
    uint32_t committed, QolSlotAdminResult* out_result);

struct QolSlotDataAdminApi {
    QolAbiHeader abi;
    QolSlotAdminOpenFn open;
    QolSlotAdminBeginFn begin;
    QolSlotAdminReadContainerFn read_container;
    QolSlotAdminImportContainerFn import_container;
    QolSlotAdminDeleteContainerFn delete_container;
    QolSlotAdminRestoreContainerFn restore_container;
    QolSlotAdminEndFn end;
};

}  // namespace qol::contracts
