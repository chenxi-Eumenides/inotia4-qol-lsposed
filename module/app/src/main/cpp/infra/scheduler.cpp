#include "infra/scheduler.h"

// 帧任务调度器实现（见 infra/scheduler.h 的接口契约）。
//
// 语义照搬 legacy infra/frame/frame_task：按帧去重、interval 归一、count 递减、
// 回调返回 false 自动注销；注册顺序即派发顺序。实现独立（固定数组 + 互斥锁），
// 与 legacy 管理器零共享，未迁移消费者继续使用 legacy。

#include <mutex>

#include "infra/runtime.h"

namespace qol::infra {
namespace {

constexpr uint32_t kCapacity = 128;
constexpr uint32_t kPointCount = 2;

struct Entry {
    FrameTaskId id = 0;
    FramePoint point = FramePoint::kRenderPre;
    FrameHandler handler = nullptr;
    void* ctx = nullptr;
    int interval = 1;         // 归一化后 >=1
    int count = 0;            // 0 = 无限；>0 = 最多触发次数
    int remaining = 0;        // count>0 时有效
    int64_t next_frame = 0;   // armed 时有效
    bool armed = false;       // 首个派发周期为 false（必触发）
};

Entry g_entries[kCapacity];
uint32_t g_count = 0;
std::mutex g_mtx;
uint64_t g_next_id = 1;  // 0 保留为无效句柄

// 每点位独立按帧去重状态，初值 -1。
int64_t g_last_frame[kPointCount] = {-1, -1};

// 重入门禁：dispatch 期间同步再入（如回调内再次 dispatch）直接返回。
thread_local int g_dispatch_depth = 0;
thread_local int g_dispatch_point = -1;

class DispatchScope {
  public:
    explicit DispatchScope(FramePoint point)
        : previous_point_(g_dispatch_point), previous_depth_(g_dispatch_depth) {
        g_dispatch_point = static_cast<int>(point);
        g_dispatch_depth = previous_depth_ + 1;
    }

    ~DispatchScope() {
        g_dispatch_point = previous_point_;
        g_dispatch_depth = previous_depth_;
    }

    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;

  private:
    int previous_point_;
    int previous_depth_;
};

struct SnapshotCall {
    FrameTaskId id;
    FrameHandler handler;
    void* ctx;
};

bool valid_point(FramePoint point) {
    const int index = static_cast<int>(point);
    return index >= 0 && index < static_cast<int>(kPointCount);
}

int point_index(FramePoint point) {
    return static_cast<int>(point);
}

int find_index(FrameTaskId id) {
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_entries[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// 删除保序：后续条目前移，保证派发顺序始终等于注册顺序。
void remove_at(uint32_t index) {
    for (uint32_t i = index; i + 1 < g_count; ++i) {
        g_entries[i] = g_entries[i + 1];
    }
    --g_count;
}

// 按帧去重：帧号有效（>0）且与上次派发帧号不同才派发（与 legacy 同语义）。
bool should_dispatch(int64_t frame, int64_t last) {
    return frame > 0 && frame != last;
}

int normalize_interval(int interval) {
    return interval > 0 ? interval : 1;
}

// 到期判定：首个派发周期（armed==false）必触发；此后 frame >= next_frame。
bool is_due(int64_t frame, int64_t next_frame, bool armed) {
    if (!armed) return true;
    return frame >= next_frame;
}

}  // namespace

FrameTaskId scheduler_add(FramePoint point, FrameHandler handler, void* ctx, int interval, int count) {
    if (handler == nullptr || !valid_point(point)) return 0;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_count >= kCapacity) return 0;

    const FrameTaskId id = g_next_id++;
    if (id == 0) return 0;  // 句柄空间耗尽（理论不可达）

    Entry& entry = g_entries[g_count];
    entry.id = id;
    entry.point = point;
    entry.handler = handler;
    entry.ctx = ctx;
    entry.interval = normalize_interval(interval);
    entry.count = count;
    entry.remaining = count;  // count==0 时无意义（无限）
    entry.next_frame = 0;
    entry.armed = false;      // 首个派发周期必触发
    ++g_count;
    return id;
}

bool scheduler_remove(FrameTaskId id) {
    if (id == 0) return false;
    std::lock_guard<std::mutex> lock(g_mtx);
    const int index = find_index(id);
    if (index < 0) return false;
    remove_at(static_cast<uint32_t>(index));
    return true;
}

void scheduler_dispatch(FramePoint point) {
    if (!valid_point(point)) return;
    if (g_dispatch_depth > 0) return;  // 防同步再入

    const int64_t frame = frame_count();
    SnapshotCall calls[kCapacity];
    uint32_t call_count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        const int index = point_index(point);
        if (!should_dispatch(frame, g_last_frame[index])) return;
        g_last_frame[index] = frame;

        for (uint32_t i = 0; i < g_count; ++i) {
            Entry& entry = g_entries[i];
            if (entry.point != point) continue;
            if (!is_due(frame, entry.next_frame, entry.armed)) continue;

            // 持锁推进调度状态，回调留到锁外。
            entry.armed = true;
            entry.next_frame = frame + entry.interval;
            if (entry.count > 0 && entry.remaining > 0) --entry.remaining;
            calls[call_count].id = entry.id;
            calls[call_count].handler = entry.handler;
            calls[call_count].ctx = entry.ctx;
            ++call_count;
        }
    }

    const DispatchScope dispatch_scope(point);
    for (uint32_t i = 0; i < call_count; ++i) {
        const SnapshotCall& call = calls[i];
        bool exists = false;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            exists = find_index(call.id) >= 0;
        }
        if (!exists) continue;  // 快照后被并发删除

        const bool keep = call.handler(frame, call.ctx);
        bool remove = !keep;  // 返回 false = 任务完成，自动注销
        if (!remove) {
            std::lock_guard<std::mutex> lock(g_mtx);
            const int index = find_index(call.id);
            if (index >= 0 && g_entries[index].count > 0 && g_entries[index].remaining <= 0) {
                remove = true;  // count 次已耗尽
            }
        }
        if (remove) scheduler_remove(call.id);
    }
}

bool scheduler_is_dispatching(FramePoint point) {
    return g_dispatch_depth > 0 && g_dispatch_point == static_cast<int>(point);
}

uint32_t scheduler_task_count(FramePoint point) {
    if (!valid_point(point)) return 0;
    std::lock_guard<std::mutex> lock(g_mtx);
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_entries[i].point == point) ++count;
    }
    return count;
}

void scheduler_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_count = 0;
    g_next_id = 1;
    for (uint32_t i = 0; i < kPointCount; ++i) {
        g_last_frame[i] = -1;
    }
}

}  // namespace qol::infra
