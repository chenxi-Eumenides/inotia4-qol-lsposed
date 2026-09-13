// frame_task.cpp —— 统一帧任务管理器（多触发点位 + 句柄 API）。

#include "frame_task.h"

#include "game_system.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace {

struct FrameTask {
    FramePointId point = 0;
    FrameTaskFn fn = nullptr;
    void* ctx = nullptr;
    int interval = 1;          // 归一化后 >=1
    int count = 0;             // 0 = 无限；>0 = 最多触发次数
    int remaining = 0;         // count>0 时有效：剩余触发次数
    int64_t next_frame = 0;    // armed 时有效：下一次触发帧号
    bool armed = false;        // 首个派发周期为 false（必触发）
};

std::mutex g_frame_task_mtx;
std::map<FrameTaskId, FrameTask> g_frame_tasks;  // 仅持锁访问
std::atomic<uint64_t> g_next_task_id{1};         // 0 保留为无效句柄

// 每点位独立按帧去重状态，初值 -1（仅持锁访问）。
std::array<int64_t, kFramePointCount> make_last_frames() {
    std::array<int64_t, kFramePointCount> frames{};
    frames.fill(-1);
    return frames;
}
std::array<int64_t, kFramePointCount> g_last_frame = make_last_frames();

// 重入门禁：dispatch 期间同步再入（如回调内再次 dispatch）直接返回。
thread_local int g_dispatch_depth = 0;

// 锁外回调快照：只携带调用所需的最小信息。
struct PendingCall {
    FrameTaskId id;
    FrameTaskFn fn;
    void* ctx;
};

}  // namespace

FrameTaskId frame_task_add(FramePointId point, FrameTaskFn fn, void* ctx, int interval, int count) {
    if (fn == nullptr || point < 0 || point >= kFramePointCount) return 0;

    const FrameTaskId id = g_next_task_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) return 0;  // 句柄空间耗尽（理论不可达）

    FrameTask task;
    task.point = point;
    task.fn = fn;
    task.ctx = ctx;
    task.interval = frame_task_detail::normalize_interval(interval);
    task.count = count;
    task.remaining = count;  // count==0 时无意义（无限）
    task.next_frame = 0;
    task.armed = false;      // 首个派发周期必触发

    std::lock_guard<std::mutex> lock(g_frame_task_mtx);
    g_frame_tasks.emplace(id, task);
    return id;
}

bool frame_task_remove(FrameTaskId id) {
    if (id == 0) return false;
    std::lock_guard<std::mutex> lock(g_frame_task_mtx);
    return g_frame_tasks.erase(id) > 0;
}

bool frame_task_query(FrameTaskId id, FrameTaskStatus* out) {
    if (out == nullptr) return false;
    *out = FrameTaskStatus{};
    if (id == 0) return false;

    std::lock_guard<std::mutex> lock(g_frame_task_mtx);
    const auto it = g_frame_tasks.find(id);
    if (it == g_frame_tasks.end()) return false;

    const FrameTask& task = it->second;
    out->active = true;
    out->remaining_runs = (task.count == 0) ? -1 : task.remaining;
    if (task.armed) {
        const int64_t current_frame = data_frame_count();
        const int64_t delta = task.next_frame - current_frame;
        out->frames_to_next = delta > 0 ? static_cast<int>(delta) : 0;
    } else {
        out->frames_to_next = 0;  // 首个待触发周期
    }
    return true;
}

void frame_task_dispatch(FramePointId point) {
    if (point < 0 || point >= kFramePointCount) return;
    if (g_dispatch_depth > 0) return;  // 防同步再入

    const int64_t frame = data_frame_count();

    std::vector<PendingCall> calls;
    {
        std::lock_guard<std::mutex> lock(g_frame_task_mtx);
        if (!frame_task_detail::should_dispatch(frame, g_last_frame[point])) return;
        g_last_frame[point] = frame;

        for (auto& entry : g_frame_tasks) {
            FrameTask& task = entry.second;
            if (task.point != point) continue;
            if (!frame_task_detail::is_due(frame, task.next_frame, task.armed)) continue;

            // 推进调度状态（持锁），回调本身留到锁外。
            task.armed = true;
            task.next_frame = frame + task.interval;
            if (task.count > 0 && task.remaining > 0) --task.remaining;

            calls.push_back(PendingCall{entry.first, task.fn, task.ctx});
        }
    }

    g_dispatch_depth++;
    for (const PendingCall& call : calls) {
        bool exists = false;
        {
            std::lock_guard<std::mutex> lock(g_frame_task_mtx);
            exists = g_frame_tasks.find(call.id) != g_frame_tasks.end();
        }
        if (!exists) continue;  // 快照后被并发删除

        const bool keep = call.fn(frame, call.ctx);
        bool remove = !keep;  // 返回 false = 任务完成，自动注销
        if (!remove) {
            std::lock_guard<std::mutex> lock(g_frame_task_mtx);
            const auto it = g_frame_tasks.find(call.id);
            if (it != g_frame_tasks.end() && it->second.count > 0 && it->second.remaining <= 0) {
                remove = true;  // count 次已耗尽
            }
        }
        if (remove) frame_task_remove(call.id);
    }
    g_dispatch_depth--;
}
