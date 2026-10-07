#pragma once

#include <cstdint>

#include "../host_sdk.h"
#include "../result.h"
#include "../save_file_evidence.h"
#include "../slot_operation.h"
#include "ports.h"

namespace qol::contracts {

// 进档转换种类；confirm_new 表示新档二次确认路径。
enum class QolSaveTransitionKind : uint32_t { kResume = 0, kBeginNew = 1, kConfirmNew = 2 };

// Adapter 观测的原版世界状态。world_ready 是原版角色就绪，不等于模块 session.ready；
// new_game_flag 为原版新档流程标志，当前无可靠原版观测点，Adapter 恒返回 0。
struct QolSaveAdapterState {
    uint32_t size;
    int32_t current_slot;
    uint32_t world_ready;
    uint32_t new_game_flag;
};

// Adapter 调用结果。QolResult::ok 只表示 Adapter 调用完成，不等于原版保存成功；
// original_result 原样保留原版真实 int 返回；未执行原版时 invoked=0 且 result_valid=0。
struct QolSaveOriginalResult {
    uint32_t size;
    uint32_t invoked;
    uint32_t result_valid;
    int32_t original_result;
};

struct QolSaveTransitionCall {
    uint32_t size;
    uint32_t kind;        // QolSaveTransitionKind
    int32_t target_slot;
    int32_t class_index;  // 非 API 选职业路径 = -1
    uint32_t reserved;
    uint64_t transition;
};

// framework_backup 是 Loader 交还的代码 trampoline，不是游戏对象；只在 resource-ready
// 装配回调接受，常数时间绑定。bind_backup 后 call 才可用；未绑定时 call 返回 NotReady
// （当前生产态即如此，惰性）。
struct QolSaveTransactionPort {
    QolAbiHeader abi;
    void* adapter_ctx;
    QolResult (*read_state)(void*, QolSaveAdapterState*);
    QolResult (*bind_backup)(void*, uint32_t entry_kind, void* framework_backup);
    QolResult (*call_original_save)(void*, const QolSlotOperationLease*, int32_t expected_slot,
                                    QolSaveOriginalResult*);
    QolResult (*call_transition)(void*, const QolSlotOperationLease*, const QolSaveTransitionCall*,
                                 QolSaveOriginalResult*);
    QolResult (*read_evidence)(void*, const QolSlotOperationLease*, int32_t slot,
                               QolSaveFileEvidence*);
};

static_assert(sizeof(QolSaveAdapterState) == 16, "adapter state layout");
static_assert(sizeof(QolSaveOriginalResult) == 16, "original result layout");
static_assert(sizeof(QolSaveTransitionCall) == 32 && offsetof(QolSaveTransitionCall, transition) == 24,
              "transition call layout");

}  // namespace qol::contracts
