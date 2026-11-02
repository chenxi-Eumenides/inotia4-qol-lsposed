#pragma once

#include <cstdint>

#include "game_ports/save_transaction_port.h"
#include "host_sdk.h"
#include "owner_token.h"
#include "slot_data.h"

namespace qol::contracts {

enum class QolSaveCallKind : uint32_t {
    kSave = 0, kResume = 1, kBeginNew = 2, kConfirmNew = 3, kMaintenance = 4,
    // 取消未决转换（原版未启动/明确失败的出口）：call.transition 携带待取消 token。
    kCancelTransition = 5,
};
enum class QolSaveOrigin : uint32_t { kOrdinary = 0, kVerifiedLoadRewrite = 1 };
enum class QolSaveState : uint32_t { kPending = 0, kDone = 1, kFailed = 2, kCancelled = 3 };

// Game 层 thunk 组装的调用描述。invocation 由 Game 层分配、仅当前回调有效；
// origin 不接受 HTTP/插件自报，verified_load_rewrite 只能由持有 invocation 分配权的
// Game 层 thunk 产生。
struct QolSaveNativeCall {
    uint32_t size;
    uint32_t kind;        // QolSaveCallKind
    int32_t target_slot;
    int32_t class_index;
    uint32_t origin;      // QolSaveOrigin
    uint32_t reserved;
    uint64_t invocation;
    uint64_t transition;
};

// core.save 汇报的领域状态；outcome 使用 QolSlotDataOutcome 值域。
struct QolSaveDomainStatus {
    uint32_t size;
    uint32_t state;    // QolSaveState
    uint32_t outcome;  // QolSlotDataOutcome 值域
    uint32_t reserved;
    int32_t slot;
    uint32_t reserved2;
    uint64_t epoch;
    uint64_t transaction;
    uint64_t transition;
    QolSaveOriginalResult original;
};

// core.save 注册的领域回调。全部主线程、宿主锁外执行；prepare_stop 在存在
// pending/unresolved 事务时返回 Busy，宿主生命周期操作据此明确失败，不伪装停用成功。
struct QolSaveOwnerCallbacks {
    QolAbiHeader abi;
    void* user;
    QolResult (*native_call)(void*, const QolSaveNativeCall*, QolSaveDomainStatus*);
    QolResult (*render_ready)(void*, uint64_t, const QolSaveAdapterState*, QolSaveDomainStatus*);
    QolResult (*prepare_stop)(void*);
};

struct QolSaveRouteApi {
    QolAbiHeader abi;
    void* host_ctx;
    // bind 复制回调表，不在调用栈访问游戏或回调；同 owner 重复 bind 返回 Busy。
    QolResult (*bind)(void*, QolOwnerToken, const QolSaveOwnerCallbacks*, uint64_t*);
    // unbind 非阻塞：NotReady = 仍在 drain（帧驱动清理），kOk = 已摘除；
    // 返回后保证不再以该 binding 调用 user。未知 id 返回 NotFound，owner 不匹配 Blocked。
    QolResult (*unbind)(void*, QolOwnerToken, uint64_t);
    // P3c 尾部追加（按 abi.size 协商，旧实现此字段为空）：hook wrapper / Game 层 thunk
    // 的统一派发入口。宿主只验证 owner 归属并转接到该 owner bind 的 native_call；
    // 派发在宿主锁外执行。binding 不存在返回 NotFound，owner 未授权返回 Blocked。
    QolResult (*dispatch)(void*, QolOwnerToken, const QolSaveNativeCall*,
                          QolSaveDomainStatus*);
};

}  // namespace qol::contracts
