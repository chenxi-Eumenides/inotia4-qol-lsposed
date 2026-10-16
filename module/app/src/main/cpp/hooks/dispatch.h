#pragma once

#include <cstdint>

#include "contracts/text_layer.h"

// 共享资源分派协议（ARCHITECTURE.md §4.5）。L2 机制层：不依赖 patch/game。
//
// 共享资源（如 MEMORYTEXT_GetText 文本热点）由唯一 owner 持 wrapper；消费者不挂 hook，
// 而是把回调登记进本模块，由 owner 的 wrapper 调 dispatch_run_text 统一分派。
//
// 逻辑名（resource_id）而非地址作为键；每个回调每次分派只调用一次；容量 32/资源。

namespace qol::hooks {

enum class ConsumerKind : int32_t { kObserve = 0, kModify = 1, kIntercept = 2 };
enum class DispatchPhase : int32_t { kBefore = 0, kAfter = 1 };

// 枚举数值是跨边界契约（contracts/text_layer.h），此处断言两处不漂移。
static_assert(static_cast<int32_t>(DispatchPhase::kBefore) == contracts::kTextPhaseBefore,
              "DispatchPhase::kBefore 必须等于 contracts::kTextPhaseBefore");
static_assert(static_cast<int32_t>(DispatchPhase::kAfter) == contracts::kTextPhaseAfter,
              "DispatchPhase::kAfter 必须等于 contracts::kTextPhaseAfter");
static_assert(static_cast<int32_t>(ConsumerKind::kObserve) == contracts::kTextConsumerObserve,
              "ConsumerKind::kObserve 必须等于 contracts::kTextConsumerObserve");
static_assert(static_cast<int32_t>(ConsumerKind::kModify) == contracts::kTextConsumerModify,
              "ConsumerKind::kModify 必须等于 contracts::kTextConsumerModify");
static_assert(static_cast<int32_t>(ConsumerKind::kIntercept) == contracts::kTextConsumerIntercept,
              "ConsumerKind::kIntercept 必须等于 contracts::kTextConsumerIntercept");

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

// ---- 布尔门型 Intercept（D2c-2b-1，§4.5 扩展）------------------------------------------
//
// 场景：资源入口已被唯一 owner hook（同址不可二次 NativeHook），其余功能域只能登记一个
// 布尔谓词，由 owner 的 wrapper 在入口查询（例：UIEquip_ButtonDestroyExe 出售门禁，
// owner=plugins/inventory_hooks，谓词由 plugins/special_equip 登记）。
// 键为逻辑 resource_id（与 owner 的 claim id 一致）；同一资源最多 1 个；容量 8。
// 线程纪律同本模块：set/clear 可来自任意线程；blocks 在 owner wrapper（游戏主线程）执行
// 回调，先持锁快照再锁外调用。
//
// 命名说明：本组 API 的 "intercept" 指「布尔门拦截」，与 ConsumerKind::kIntercept
// （文本链短路消费者，返回替换文本）语义不同，二者键空间与实现互相独立。
using InterceptGateFn = bool (*)(void* ctx);

// 登记布尔门：同 resource_id 已登记同一 fn → 幂等 true；已登记不同 fn / 空参 / 容量满 → false。
bool dispatch_set_intercept(const char* resource_id, InterceptGateFn fn, void* ctx);
// 移除登记；未登记返回 false。
bool dispatch_clear_intercept(const char* resource_id);
// 查询：未登记返回 false；已登记调用 fn(ctx != nullptr ? ctx : 注册时 ctx)。
bool dispatch_intercept_blocks(const char* resource_id, void* ctx);

void dispatch_reset_for_test();

}  // namespace qol::hooks
