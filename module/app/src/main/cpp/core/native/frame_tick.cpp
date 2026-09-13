// frame_tick.cpp —— 主线程逐帧派发宿主（自动出售阶段 A 基础设施）。

#include "frame_tick.h"

#include "game_system.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace {

struct Tick {
    FrameTickFn fn;
    void* ctx;
};

std::mutex g_frame_tick_mtx;
std::vector<Tick> g_frame_ticks;
int64_t g_last_dispatched_frame = -1;  // 仅持锁访问

}  // namespace

bool frame_tick_register(FrameTickFn fn, void* ctx) {
    if (fn == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_frame_tick_mtx);
    for (const Tick& t : g_frame_ticks) {
        if (t.fn == fn && t.ctx == ctx) return true;  // 幂等
    }
    g_frame_ticks.push_back({fn, ctx});
    return true;
}

void frame_tick_unregister(FrameTickFn fn, void* ctx) {
    std::lock_guard<std::mutex> lock(g_frame_tick_mtx);
    for (auto it = g_frame_ticks.begin(); it != g_frame_ticks.end();) {
        if (it->fn == fn && it->ctx == ctx) {
            it = g_frame_ticks.erase(it);
        } else {
            ++it;
        }
    }
}

int64_t frame_tick_current_frame() {
    return data_frame_count();
}

void frame_tick_dispatch() {
    const int64_t frame = data_frame_count();
    std::vector<Tick> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_frame_tick_mtx);
        if (!frame_tick_detail::should_dispatch(frame, g_last_dispatched_frame)) return;
        g_last_dispatched_frame = frame;
        snapshot = g_frame_ticks;  // 持锁快照，锁外执行回调
    }
    for (const Tick& t : snapshot) {
        t.fn(t.ctx);
    }
}
