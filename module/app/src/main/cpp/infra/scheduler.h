#pragma once

#include <cstdint>

// 统一帧任务调度器（ARCHITECTURE.md §6.1：跨线程访问矩阵 / 主线程派发）。
//
// 帧源（host/frame_source）在游戏主线程帧锚点调用 scheduler_dispatch(point)；
// 同一 (point, frame) 每帧只派发一次；回调收到当前帧号并在派发线程执行。
//
// 帧号经 infra::runtime 已注入的 Clock 读取：infra 不依赖 game（§2.2），
// 未注入（frame_count() 返回 -1）或帧号 <= 0 时跳过派发。
//
// 线程模型：注册/删除/查询可来自任意线程（共享一把互斥锁）；回调只在游戏主线程
// 执行。注册顺序即派发顺序；回调返回 false 或 count 耗尽自动注销。
//
// 与 legacy infra/frame/frame_task 并存：未迁移消费者仍用 legacy 管理器，
// 帧锚点 wrapper 同时派发两者；本实现独立，不共享状态。

namespace qol::infra {

enum class FramePoint : int32_t { kRenderPre = 0, kLogicPre = 1 };

using FrameTaskId = uint64_t;  // 0 = 无效

// 回调：frame = 当前帧号；ctx = 注册上下文。返回 false = 任务完成，自动注销。
using FrameHandler = bool (*)(int64_t frame, void* ctx);

// 注册帧任务，返回句柄（0=失败）。
//   interval：<=0 归一为 1（每帧）；>0 = 每 interval 帧触发一次。
//   count：0=一直触发；>0=最多触发 count 次。
// 容量 128；满或 handler 为空或 point 越界返回 0。
FrameTaskId scheduler_add(FramePoint point, FrameHandler handler, void* ctx, int interval, int count);

// 注销帧任务；未知 id 返回 false（幂等由调用方以返回值为准）。
bool scheduler_remove(FrameTaskId id);

// 由帧源（host）调用：按注册顺序派发该点位到期任务；帧号取 infra::frame_count()
// （未注入 → -1，跳过派发）。
void scheduler_dispatch(FramePoint point);

// True only while this thread is executing a callback from the requested frame-point dispatch.
bool scheduler_is_dispatching(FramePoint point);

uint32_t scheduler_task_count(FramePoint point);

void scheduler_reset_for_test();

}  // namespace qol::infra
