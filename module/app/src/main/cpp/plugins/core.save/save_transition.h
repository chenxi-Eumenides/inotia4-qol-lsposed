#pragma once

#include <cstdint>

#include "contracts/host_sdk.h"

namespace qol::features::core_save {

// ---- 选角/进档转换状态机（fake 驱动；生产接线 P3c）----
enum class TransitionKind : uint32_t { kResume = 0, kBeginNew = 1 };
enum class TransitionPhase : uint32_t {
    kDetached = 0,       // 旧 session 准入已撤销，等待选角。
    kSelecting = 1,      // 选角界面（API 路径同相位）。
    kExecuting = 2,      // 原版转换执行中。
    kAwaitingWorld = 3,  // 原版完成，等待 RenderPre 观测世界就绪。
    kConfirming = 4,     // 确认序执行中。
    kTerminal = 5,
};

QolResult save_transition_initialize(contracts::QolOwnerToken owner,
                                     contracts::QolOwnerVerifyTokenFn verify,
                                     const contracts::QolOwnerStoreApi* backend,
                                     const contracts::QolSlotOperationApi* operation,
                                     int32_t (*is_main_thread)());
void save_transition_shutdown();
// 领域健康缝（协调器注入）：pending/degraded 槽拒绝开始转换；未注入视为允许。
void save_transition_set_transition_gate(bool (*allowed)(int32_t slot, void* ctx), void* ctx);
// journal 分派缝（协调器注入）：confirm 序对未决 journal 的严格分派；返回非 ok 即阻断
// 且不消费 pending。未注入时任何未决 journal 均阻断（保守，不抹证据）。
void save_transition_set_journal_verdict(QolResult (*verdict)(int32_t slot, void* ctx), void* ctx);

// requested_slot 是已校验的请求槽（API 读档/建档直接采用，不读 current_slot 代替）。
// begin 原子取得覆盖 old+target 两槽的 operation lease；关闭旧 session 准入；
// prepare_detach 在全部基础设施锁之外执行。同槽重入分配新 token 与新 epoch。
QolResult save_transition_begin(uint32_t kind, int32_t requested_slot, uint64_t* out_token);
// RenderPre 确认序：按 token id 显式复用（禁止按槽相同查找）；验证 token+观测槽 →
// 新档 reset / 读档 ensure + 未决 journal 严格分派 → 新 epoch → session_ready 通知 →
// SessionBound 加载 / Immediate 排空 → ready 发布。任一步失败不消费 pending、不派发
// 后续回调/事件、保持阻断。
QolResult save_transition_confirm(uint64_t token_id, int32_t observed_slot);
// 取消选角：只释放 token 与 lease，不伪装恢复已删 dat。
QolResult save_transition_cancel(uint64_t token_id);
// 确认序成功发布 ready 之后才允许旧 save_enter 回调与 kEventSaveEnter；新转换开始即撤销。
bool save_transition_ready_for_save_enter();
// 宿主生命周期停用前置：存在未决转换或未决事务时 Busy。
QolResult save_transition_prepare_stop();
void save_transition_reset_for_test();

}  // namespace qol::features::core_save
