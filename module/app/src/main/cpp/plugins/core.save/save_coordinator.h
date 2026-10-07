#pragma once

#include <cstdint>
#include <string>

#include "contracts/game_ports/save_transaction_port.h"
#include "contracts/host_sdk.h"

namespace qol::features::core_save {

// ---- 唯一联合保存协调器（fake 驱动；生产接线 P3c）----
// 线性事务：固定参与者集合 → 恰一次 capture → 三候选真实编码预检 →
// acquire operation → reserve_in_operation → prepared journal → referenced 双凭据 →
// call_original_save → read_evidence → confirmed journal → 一次 replace_reserved →
// complete(committed) → 释放 reservation → 释放 operation。
// 失败路径冻结：capture/预检/prepared 失败不调原版（cancelled）；原版有效返回 0 不替换
// 正式 sections、条件清理 journal（否则 degraded 留证）；evidence/confirmed/replace 失败
// 进 commit-pending 保留 referenced 双凭据与捕获字节，同 tx 重试 ≤3 次不重 capture 不重调
// 原版，耗尽 degraded。commit-pending/recovery-retrying/degraded 是每槽私有健康态，不进 ABI；
// session 三态不被本模块改动。

QolResult save_coordinator_initialize(contracts::QolOwnerToken owner,
                                      contracts::QolOwnerVerifyTokenFn verify,
                                      const contracts::QolOwnerStoreApi* backend,
                                      const contracts::QolSlotOperationApi* operation,
                                      const contracts::QolSaveTransactionPort* adapter,
                                      int32_t (*is_main_thread)());
void save_coordinator_shutdown();
bool save_coordinator_installed();

enum class SaveSettle { kCommitted, kPending, kFailed };
// 进档/转换确认序调用：未决 journal 的严格分派与自动恢复。stage=1 且当前 dat evidence
// 匹配才自动重试同 tx replace；prepared/v1/未知/损坏一律 degraded 保留证据。
void save_coordinator_on_session_enter(int32_t slot, uint64_t frame);
// 帧驱动重试（commit-pending/recovery-retrying；不重新 capture、不重调原版）。
SaveSettle save_coordinator_retry_frame(int32_t slot, uint64_t frame, std::string* out_error);
bool save_coordinator_permits_transition(int32_t slot);
bool save_coordinator_permits_save(int32_t slot);

// HTTP 结算：attempt==0 执行新事务，后续 attempt 只查询/推进该槽已绑定的 tx；
// sidecar 正式提交确认前返回 kPending（票据保持 pending），耗尽/degraded 给失败终态。
// out_body 携带冻结成功体 {"ok":true,"slot":S,"sidecar":"ensured"} 或错误体。
SaveSettle save_coordinator_settle(int32_t slot, uint32_t attempt, uint64_t frame,
                                   std::string* out_body);
void save_coordinator_reset_for_test();

}  // namespace qol::features::core_save
