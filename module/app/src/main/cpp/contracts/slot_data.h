#pragma once

#include <cstdint>

#include "owner_token.h"
#include "host_sdk.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kQolSlotDataCapabilityId = "save.slot_data";
constexpr QolVersion kQolSlotDataVersion{1, 0};
// 沿用 infra/store.cpp 的预算，不提高存储上限。
constexpr uint32_t kQolSlotDataMaxSectionNameBytes = 64;
constexpr uint64_t kQolSlotDataMaxPayloadBytes = 1024 * 1024;
constexpr uint64_t kQolSlotDataMaxContainerBytes = 4 * 1024 * 1024;

using QolSlotDataHandle = uint64_t;
using QolSlotDataRequest = uint64_t;
constexpr QolSlotDataHandle kQolSlotDataHandleInvalid = 0;
constexpr QolSlotDataRequest kQolSlotDataRequestInvalid = 0;

// 读写为两个独立维度，所有组合均按槽隔离，仅触发方式不同。
enum class QolSlotDataReadPolicy : uint32_t {
    kImmediate = 0,     // 消费者自行决定何时请求加载。
    kSessionBound = 1,  // core.save 在 session 就绪时自动加载。
};
enum class QolSlotDataWritePolicy : uint32_t {
    kImmediate = 0,  // 提交修改请求时即落盘，不等原版保存。
    kSlotBound = 1,  // 只标 dirty，原版保存事务成功后统一提交。
};
enum class QolSlotSessionState : uint32_t {
    kUnavailable = 0, kActivating = 1, kReady = 2, kClosing = 3,
};
enum class QolSlotDataReason : uint32_t {
    kEnter = 0, kCatchUp = 1, kRequested = 2, kSave = 3, kRecovery = 4,
};
// IO 不可用、损坏、版本不支持不得全部伪装为 missing。
enum class QolSlotDataPresence : uint32_t {
    kPresent = 0, kMissing = 1, kUnreadable = 2, kVersionUnsupported = 3,
};
enum class QolSlotDataRequestState : uint32_t {
    kPending = 0, kCompleted = 1, kFailed = 2, kCancelled = 3,
};
// kSlotBound 写在原版保存成功前是 staged，不等于 durable。
enum class QolSlotDataDurability : uint32_t {
    kStaged = 0, kDurable = 1, kAbsent = 2,
};
enum class QolSlotDataOutcome : uint32_t {
    kCommitted = 0,           // 正式 section 已提交。
    kStaged = 1,              // 已标 dirty，等待原版保存成功。
    kOriginalSaveFailed = 2,  // 原版保存失败，不提交。
    kPendingRecovery = 3,     // commit-pending，等待恢复重试。
    kDegraded = 4,            // 恢复失败或证据不足。
    kCancelled = 5,           // 事务在 prepare 阶段被取消。
};

struct QolSlotSession {
    int32_t slot;
    uint32_t state;  // QolSlotSessionState
    uint64_t epoch;  // 每次进档递增，同槽重入也递增。
};
struct QolSlotDataContext {
    QolSlotSession session;
    uint32_t reason;       // QolSlotDataReason
    uint64_t transaction; // 非保存回调为 0。
};
struct QolSlotDataStatus {
    uint32_t size;        // 输出结构体大小，供 ABI 尾部扩展。
    uint32_t state;       // QolSlotDataRequestState
    uint32_t durability;  // QolSlotDataDurability
    uint32_t reason;      // QolSlotDataReason
    uint32_t presence;    // QolSlotDataPresence
    uint32_t stored_version;
    int32_t slot;
    uint64_t epoch;
    QolResult result;
};

// 所有消费者回调在游戏主线程执行。
// 幂等清理旧 session，可拒绝；拒绝则阻止原版转换。
using QolSlotDataPrepareDetachFn = QolResult (*)(void* user,
    const QolSlotSession* old_session, uint32_t reason);
// bytes 仅本次调用有效；missing 由消费者生成默认状态，不可读不能当作缺失。
using QolSlotDataLoadFn = QolResult (*)(void* user, const QolSlotDataContext* ctx,
    uint32_t presence, uint32_t stored_version, const uint8_t* bytes, uint64_t size);
// 单次调用完成序列化，不能两次调用先测长度再写而重复业务 prepare。
using QolSlotDataCaptureFn = QolResult (*)(void* user, const QolSlotDataContext* ctx,
    uint8_t* out_buffer, uint64_t capacity, uint64_t* out_size);
// 仅幂等处理内存记账，不再执行 section IO。
using QolSlotDataCompleteFn = void (*)(void* user, const QolSlotDataContext* ctx,
    uint32_t outcome);
// session 就绪通知；kSessionBound 的 load 在其后同轮调用。
// kImmediate 消费者收到通知后自行 request_load；尚未加载不得恢复数据操作。
using QolSlotDataSessionReadyFn = QolResult (*)(void* user, const QolSlotDataContext* ctx);

struct QolSlotDataSpec {
    uint32_t size;
    QolOwnerToken owner;
    const char* section;
    uint16_t section_version;
    uint32_t read_policy;   // QolSlotDataReadPolicy
    uint32_t write_policy;  // QolSlotDataWritePolicy
    void* user;
    QolSlotDataPrepareDetachFn prepare_detach;
    QolSlotDataSessionReadyFn session_ready;
    QolSlotDataLoadFn load;
    QolSlotDataCaptureFn capture;
    QolSlotDataCompleteFn complete;
};

// 仅复制规格，不在注册调用栈里加载或回调。kSessionBound 自动申请当前 session
// 补加载并返回 out_initial_request；不需要时置 kQolSlotDataRequestInvalid。
using QolSlotDataRegisterFn = QolResult (*)(const QolSlotDataSpec* spec,
    QolSlotDataHandle* out_handle, QolSlotDataRequest* out_initial_request);
// 请求不接受 slot/section，仅操作注册绑定的 section 与当前 session。
// expected_epoch 不匹配则拒绝，防止旧面板把数据写入新档。
// kImmediate 在主线程且无事务占用时当次执行；其他线程投递主线程返回 request。
// 「立刻」是不等下一次原版读写存档，不允许后台线程直接操作游戏。
using QolSlotDataRequestLoadFn = QolResult (*)(QolSlotDataHandle handle,
    uint64_t expected_epoch, QolSlotDataRequest* out_request);
using QolSlotDataRequestWriteFn = QolResult (*)(QolSlotDataHandle handle,
    uint64_t expected_epoch, QolSlotDataRequest* out_request);
// 非阻塞注销：先关闭准入、拒绝新请求，等待已有回调及捕获事务释放引用，
// 不阻塞主线程。句柄含注册代次，注销完成后不可复用。
// 返回 request 的 query 报终态后，provider 保证不再触发任何消费者回调，
// 消费者据此释放 user 与回调资源，避免在途引用悬空。
// 停用与注销保留磁盘 section，不自动删除，也不隐式保存。
using QolSlotDataUnregisterFn = QolResult (*)(QolSlotDataHandle handle,
    QolSlotDataRequest* out_request);
// 线程安全；注销 request 的终态是释放 user 与回调资源的依据。
using QolSlotDataQueryFn = QolResult (*)(QolSlotDataRequest request,
    QolSlotDataStatus* out_status);
using QolSlotDataSessionFn = QolResult (*)(QolSlotSession* out_session);

struct QolSlotDataApi {
    QolAbiHeader abi;
    QolSlotDataRegisterFn register_section;
    QolSlotDataRequestLoadFn request_load;
    QolSlotDataRequestWriteFn request_write;
    QolSlotDataUnregisterFn unregister;
    QolSlotDataQueryFn query;
    QolSlotDataSessionFn session;
};

}  // namespace qol::contracts
