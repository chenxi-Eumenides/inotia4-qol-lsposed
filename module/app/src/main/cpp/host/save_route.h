#pragma once

#include "contracts/save_route.h"

namespace qol::host::save_route {

// 帧驱动 drain（主线程逻辑帧点调用）：清理 unbind 后等待在途回调归零的 binding。
void step();

// Game 层 thunk（P3c 接线）经此转接到 core.save 注册的回调；宿主只验证归属并转接，
// 不持有领域时序。回调在宿主锁外执行。binding 不存在返回 NotFound；已 unbind 的
// binding 不再派发。
QolResult dispatch_native_call(contracts::QolOwnerToken owner,
                               const contracts::QolSaveNativeCall* call,
                               contracts::QolSaveDomainStatus* out_status);
QolResult dispatch_render_ready(contracts::QolOwnerToken owner, uint64_t transition,
                                const contracts::QolSaveAdapterState* adapter_state,
                                contracts::QolSaveDomainStatus* out_status);

// 宿主生命周期停用前的转接：透传 owner 的 prepare_stop；Busy 表示存在
// pending/unresolved 事务，宿主生命周期操作必须明确失败。
QolResult prepare_stop(contracts::QolOwnerToken owner);

// Host SDK 面（QolHostSdk.save_route 指向的表）；bind 授权在 route 内经 owner 身份校验。
const contracts::QolSaveRouteApi& save_route_api();

// owner_identity suspend 通知：旧 token 的 binding 退出派发，等在途回调归零后清理。
void owner_stopped(uint64_t token);

void reset_for_test();

}  // namespace qol::host::save_route
