// game_motion.cpp —— 由 game_data.cpp 拆分生成（纯搬代码，零逻辑变更）

#include "game_system.h"

#include "game_access.h"
#include "game_symbols.h"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define MOVE_TAG "Inotia4Move"
#define MOVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, MOVE_TAG, __VA_ARGS__)
#include "game_motion.h"
#include "game_state.h"

struct FrameTask {
    bool (*fn)(void*);  // 返回 true 继续，false 完成
    void* ctx;          // 任务上下文（角色指针/方向/剩余帧等）
    int id;
};

std::mutex g_task_mtx;
std::vector<FrameTask> g_tasks;
std::thread g_task_thread;
std::atomic<bool> g_task_running{false};
std::atomic<bool> g_task_stop{false};
int g_task_next_id = 1;

void task_thread_fn();

// 注册帧任务，返回任务 id（0=失败）
int frame_task_register(bool (*fn)(void*), void* ctx) {
    std::lock_guard<std::mutex> lock(g_task_mtx);
    if (fn == nullptr || !game_in_world()) return 0;
    // v0.4.37：剧情/切图（GAMESTATE_nState!=0）期间禁止注册新任务——此时注册会立即在
    // 后台线程调 CHAR_Move 与剧情/切图状态机竞争，破坏游戏控制态导致结束后卡死。
    if (g_gamestate != nullptr && *reinterpret_cast<uint32_t*>(g_gamestate) != 0) return 0;
    g_tasks.clear();  // 单任务语义：注册即替换
    FrameTask t{fn, ctx, g_task_next_id++};
    g_tasks.push_back(t);
    if (!g_task_running.load()) {
        if (g_task_thread.joinable()) g_task_thread.join();  // 等旧线程完全退出
        g_task_running.store(true);
        g_task_thread = std::thread(task_thread_fn);
    }
    return t.id;
}

void frame_task_unregister(int id) {
    std::lock_guard<std::mutex> lock(g_task_mtx);
    if (id <= 0) {
        g_tasks.clear();
        return;
    }
    for (auto it = g_tasks.begin(); it != g_tasks.end(); ++it) {
        if (it->id == id) { g_tasks.erase(it); return; }
    }
}

void stop_all_tasks() {
    g_task_stop.store(true);
    if (g_task_thread.joinable()) g_task_thread.join();
    g_task_stop.store(false);
    std::lock_guard<std::mutex> lock(g_task_mtx);
    g_tasks.clear();
}

void task_thread_fn() {
    uint64_t last_frame = 0;
    int step = 0;
    for (;;) {
        if (g_task_stop.load()) break;
        int64_t f = data_frame_count();
        if (f > 0 && static_cast<uint64_t>(f) != last_frame) {
            last_frame = f;
            std::vector<FrameTask> snapshot;
            {
                std::lock_guard<std::mutex> lock(g_task_mtx);
                if (g_tasks.empty()) break;
                snapshot = g_tasks;
            }
            for (const FrameTask& t : snapshot) {
                bool cont = t.fn(t.ctx);
                MOVE_LOG("task %d step %d cont=%d", t.id, step++, cont);
                if (!cont) frame_task_unregister(t.id);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_task_running.store(false);
}
