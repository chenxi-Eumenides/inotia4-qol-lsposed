#pragma once

#include <cstdint>

// 统一帧任务管理器（多触发点位 + 句柄 API）。
//
// 设计：锚点 wrapper（core/native/frame_host）在游戏主线程帧周期调用
// frame_task_dispatch(point)；同一 (point, frame) 每帧只派发一次；回调统一在调用
// dispatch 的线程（游戏主线程）执行并收到当前 frame 数。
//
// 线程模型：注册/删除/查询可被任意线程调用（AndServer 工作线程、bridge-init、hook
// 线程、UI 线程），共享一把互斥锁；回调只在游戏主线程执行。**绝不允许持锁调用回调**：
// 先持锁快照 {id, fn, ctx} 并推进 next_frame/remaining，再锁外执行；调用前再次持锁
// 确认该 id 仍存在（并发删除保护）。
//
// 不依赖任何 feature；帧号来源为 data_frame_count()。

using FramePointId = int;
enum : FramePointId {
    kFramePointRenderPre = 0,  // 渲染开始前（GAMESTATE_Draw 内 bl GAMESTATE_DrawPlay 锚点）
    kFramePointCount,          // 点位数量（API/数据结构按多点位设计，每点位独立按帧去重）
};

using FrameTaskId = uint64_t;  // 0 = 无效

// 回调：frame = 当前帧号；ctx = 注册上下文。返回 false = 任务完成，自动注销。
using FrameTaskFn = bool (*)(int64_t frame, void* ctx);

struct FrameTaskStatus {
    bool active = false;
    int remaining_runs = 0;  // 剩余触发次数；count==0（无限）时返回 -1
    int frames_to_next = 0;  // active 时 = max(0, next_frame - data_frame_count())
};

// 注册帧任务，返回句柄（0=失败）。
//   interval：0=每帧（内部归一为 1）；>0=每 interval 帧触发一次。
//   count：0=一直触发；>0=最多触发 count 次。
// 语义：注册后的首个派发周期必触发（不补发注册前的帧），其后
// next_frame = frame + max(1, interval)。fn 为空或 point 越界返回 0。
FrameTaskId frame_task_add(FramePointId point, FrameTaskFn fn, void* ctx, int interval, int count);

// 注销帧任务；幂等。未知 id 返回 false。
bool frame_task_remove(FrameTaskId id);

// 查询帧任务状态；未知/已注销 id 返回 false（out->active=false）。
bool frame_task_query(FrameTaskId id, FrameTaskStatus* out);

// 由锚点 wrapper 在游戏主线程调用；同一 (point, frame) 只派发一次；
// thread_local 重入深度 >0 直接返回（防同步再入）。
void frame_task_dispatch(FramePointId point);

namespace frame_task_detail {

// 按帧去重纯函数：帧号有效（>0）且与上次派发帧号不同才派发。
constexpr bool should_dispatch(int64_t frame, int64_t last) {
    return frame > 0 && frame != last;
}

// interval 归一：0 或负值归一为 1（每帧）。
constexpr int normalize_interval(int interval) {
    return interval > 0 ? interval : 1;
}

// 到期判定：armed==false（尚未安排下一次，即首个派发周期）视为到期；
// armed==true 时 frame >= next_frame 才触发。
constexpr bool is_due(int64_t frame, int64_t next_frame, bool armed) {
    if (!armed) return true;
    return frame >= next_frame;
}

}  // namespace frame_task_detail
