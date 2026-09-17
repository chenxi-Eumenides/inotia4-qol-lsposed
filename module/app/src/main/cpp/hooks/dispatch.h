#pragma once

#include <cstdint>

// 共享资源分派协议（ARCHITECTURE.md §4.5）。L2 机制层：不依赖 patch/game。
//
// 共享资源（如 MEMORYTEXT_GetText 文本热点）由唯一 owner 持 wrapper；消费者不挂 hook，
// 而是把回调登记进本模块，由 owner 的 wrapper 调 dispatch_run_text 统一分派。
//
// 逻辑名（resource_id）而非地址作为键；每个回调每次分派只调用一次；容量 32/资源。

namespace qol::hooks {

enum class ConsumerKind : int32_t { kObserve = 0, kModify = 1, kIntercept = 2 };
enum class DispatchPhase : int32_t { kBefore = 0, kAfter = 1 };

using DispatchId = uint64_t;  // 0 = 无效

// 权限别名（同一 DispatchId 的类型化表达，§4.5「权限类型」）。
using ObserveHandle = DispatchId;
using ModifyHandle = DispatchId;
using InterceptHandle = DispatchId;

// 文本资源回调：Observe 忽略返回值；Modify 返回替换文本或 nullptr（不改）；
// Intercept 返回最终文本或 nullptr（回退当前值）。
using TextDispatchFn = const char* (*)(uint32_t text_id, const char* current, void* ctx);

// 注册消费者（阶段 + 权限，§4.5）。同一资源最多 1 个 Intercept（**跨阶段唯一**）；
// 总容量 32/资源；重名资源自动分组。返回 0 = 失败（空 resource_id/fn、非法 phase/kind、
// 超容量、重复 Intercept）。
DispatchId dispatch_subscribe(const char* resource_id, DispatchPhase phase, ConsumerKind kind,
                              TextDispatchFn fn, void* ctx);

// 注销消费者；未找到返回 false。
bool dispatch_unsubscribe(const char* resource_id, DispatchId id);

uint32_t dispatch_consumer_count(const char* resource_id, DispatchPhase phase, ConsumerKind kind);

// owner 的 wrapper 调用（§4.5 三段式，逐项对应）：
//   before（注册顺序）：该阶段 Observe 只观察（忽略返回）、Modify 非空返回链式更新 current、
//                       Intercept 命中即短路；
//   original：owner/游戏原值由调用方经 original 传入（本函数不回调原函数）；
//   after（逆序）：该阶段 Observe 观察、Modify 非空返回覆盖返回值（后注册者先覆盖）。
//   每个回调每次分派只调用一次；分派期间快照消费者（本轮注销/新增下轮生效）。
//   短路：Intercept 短路后，before 已执行部分保留；跳过 original 与 after 阶段的
//   Modify/Intercept；after 阶段的 Observe 仍执行（可观察，不可改）。
//   同资源重入直接返回 original；未注册资源直接返回 original。
const char* dispatch_run_text(const char* resource_id, uint32_t text_id, const char* original);

void dispatch_reset_for_test();

}  // namespace qol::hooks
