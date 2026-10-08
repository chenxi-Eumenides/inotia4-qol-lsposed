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

// 按插件 id 解析 owner token 后转接 dispatch_native_call；供宿主过渡缝
//（runtime_hooks）使用——patch/game 层只有插件 id，没有 owner token。
// token 查询经装配期注入缝（plugin_registry 提供），未注入时按未注册处理。
// 插件未注册返回 NotFound。
using PluginTokenQuery = contracts::QolOwnerToken (*)(const char* plugin_id);
void set_plugin_token_query(PluginTokenQuery query);
QolResult dispatch_native_call_for_plugin(const char* plugin_id,
                                          const contracts::QolSaveNativeCall* call,
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
