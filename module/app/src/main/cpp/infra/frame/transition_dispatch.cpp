// transition_dispatch.cpp —— 游戏状态转换派发器（单飞 + 同步等待 + 逻辑帧点消费）。
//
// 详见头文件语义说明。核心不变量：
//  - 游戏状态机函数只在游戏主线程（kFramePointLogicPre 帧回调）执行；
//  - 任意时刻至多一个转换在途，重复请求立即 busy，不排队（避免上次事故的线程堆积）；
//  - 持锁只保护 pending/result/seq，绝不持锁调用 fn。

#include "infra/frame/transition_dispatch.h"

#include "infra/frame/frame_host.h"
#include "infra/frame/frame_task.h"
#include "infra/log/qol_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {

struct PendingTask {
    uint64_t seq = 0;
    std::function<void(std::string*)> fn;
    bool consumed = false;  // 由游戏线程在持锁下置位：已取走，不可再取消
};

std::mutex g_mtx;
std::condition_variable g_cv;
std::shared_ptr<PendingTask> g_pending;  // 仅持锁访问
uint64_t g_seq_counter = 0;              // 仅持锁访问
uint64_t g_done_seq = 0;                 // 仅持锁访问
std::string g_result;                    // 仅持锁访问
std::atomic<bool> g_in_flight{false};
std::atomic<bool> g_tick_registered{false};
// 逻辑相位所在线程（transition_tick 每帧记录）：transition_run_inline 据此校验调用位置，
// 防止在非游戏主线程直接执行游戏状态机。
std::atomic<std::thread::id> g_logic_thread{};

// 逻辑相位消费：游戏主线程取走 pending、锁外执行、回写结果并唤醒 HTTP 线程。
bool transition_tick(int64_t /*frame*/, void* /*ctx*/) {
    g_logic_thread.store(std::this_thread::get_id(), std::memory_order_release);
    std::shared_ptr<PendingTask> task;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (!g_pending) return true;
        task = g_pending;
        g_pending.reset();
        task->consumed = true;
    }

    std::string result;
    task->fn(&result);  // 游戏线程执行，不持任何派发锁

    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_result = std::move(result);
        g_done_seq = task->seq;
    }
    g_cv.notify_all();
    return true;
}

}  // namespace

bool transition_dispatch_init() {
    if (g_tick_registered.exchange(true, std::memory_order_acq_rel)) return true;
    const FrameTaskId id = frame_task_add(kFramePointLogicPre, &transition_tick, nullptr, 0, 0);
    if (id == 0) {
        g_tick_registered.store(false, std::memory_order_release);  // 允许后续重试
        return false;
    }
    QOL_LOG_INFO(QolDomain::kPlatform, "logic-pre consumer registered");
    return true;
}

bool transition_run(std::function<void(std::string*)> fn, int timeout_ms, std::string* result) {
    if (result != nullptr) result->clear();
    if (!fn) {
        if (result != nullptr) *result = "transition function required";
        return false;
    }
    // 逻辑锚点未安装时游戏线程不会消费，直接 fail-fast（避免每次等满超时）。
    if (!frame_host_anchors_installed()) {
        if (result != nullptr) *result = "logic anchor not installed";
        return false;
    }
    // 单飞：已有转换在途则立即拒绝（不排队）。
    const bool was_in_flight = g_in_flight.exchange(true, std::memory_order_acq_rel);
    if (transition_detail::submit_decision(was_in_flight) ==
        transition_detail::SubmitDecision::kBusy) {
        if (result != nullptr) *result = "transition in progress";
        return false;
    }

    auto task = std::make_shared<PendingTask>();
    task->fn = std::move(fn);
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        task->seq = ++g_seq_counter;
        g_pending = task;
    }

    std::unique_lock<std::mutex> lock(g_mtx);
    const bool done = g_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                    [&] { return g_done_seq == task->seq; });
    if (!done) {
        if (transition_detail::on_timeout(task->consumed) ==
            transition_detail::TimeoutAction::kCancelPending) {
            g_pending.reset();  // 尚未被消费：安全取消
            g_in_flight.store(false, std::memory_order_release);
            lock.unlock();
            QOL_LOG_WARN(QolDomain::kPlatform, "transition timeout (cancelled)");
            if (result != nullptr) *result = "transition timeout";
            return false;
        }
        // 已被游戏线程取走：等待其完成，避免 fn 使用中的状态下提前返回。
        g_cv.wait(lock, [&] { return g_done_seq == task->seq; });
    }

    if (result != nullptr) *result = g_result;
    g_in_flight.store(false, std::memory_order_release);
    return true;
}

bool transition_run_inline(std::function<void(std::string*)> fn, std::string* result) {
    if (result != nullptr) result->clear();
    if (!fn) {
        if (result != nullptr) *result = "transition function required";
        return false;
    }
    if (!frame_host_anchors_installed()) {
        if (result != nullptr) *result = "logic anchor not installed";
        return false;
    }
    // 只允许逻辑相位（命令排空所在的同一线程/相位）执行：否则游戏状态机跑在错误线程。
    if (g_logic_thread.load(std::memory_order_acquire) != std::this_thread::get_id()) {
        if (result != nullptr) *result = "not in logic phase";
        return false;
    }
    // 与 transition_run 共用单飞门：不排队，抢不到立即 busy。
    const bool was_in_flight = g_in_flight.exchange(true, std::memory_order_acq_rel);
    if (transition_detail::submit_decision(was_in_flight) ==
        transition_detail::SubmitDecision::kBusy) {
        if (result != nullptr) *result = "transition in progress";
        return false;
    }
    // 已在逻辑相位：直接执行，无跨帧派发、无阻塞等待。
    fn(result);
    g_in_flight.store(false, std::memory_order_release);
    return true;
}
