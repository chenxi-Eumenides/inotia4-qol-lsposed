#pragma once

#include <functional>
#include <string>

// 模块级「游戏状态转换派发器」（修复 HTTP worker 线程直调游戏状态机导致的死锁）。
//
// 背景：go_main_menu / enter_slot / create_slot 会调用游戏状态机
// （GAMESTATE_SetState -> GAME_Exit / GAME_StartResumeGame 等）。这些函数不是线程安全的，
// 必须在游戏主线程、且在本帧逻辑处理之前的固定相位执行；HTTP worker 线程直接调用会与
// 渲染线程并发进入游戏的 MEM 分配器，破坏空闲链表并导致 MEM_Free 死循环（帧计数冻结）。
//
// 语义：
//  - 任意线程调用 transition_run(fn, timeout_ms, &result)；
//  - 同一时刻只允许一个转换在途（单飞）：抢不到立即返回 false，result 为 busy 文本；
//  - fn 在游戏主线程的 kFramePointLogicPre 相位执行（见 frame_host 逻辑锚点）；
//  - 调用方同步阻塞等待；超时且任务尚未被消费则取消（消费后不可中途取消）；
//  - fn 在游戏线程执行时把最终响应（op_ok/op_err JSON 等）写入 result；
//    transition_run 返回 true 表示 fn 已执行（result 为 fn 输出），
//    false 表示 busy/超时（result 为纯文本错误，调用方自行包 op_err）。
bool transition_run(std::function<void(std::string*)> fn, int timeout_ms, std::string* result);

// 注册逻辑相位消费帧任务（幂等）。供 nativeInit 调用；返回是否注册成功。
bool transition_dispatch_init();

namespace transition_detail {

// 单飞提交决策：先前已有转换在途 -> 拒绝（busy）；否则放行。
// 单独成函数是为了让「门」可被 host 测试覆盖，避免调用点再写反。
enum class SubmitDecision {
    kProceed,
    kBusy,
};
constexpr SubmitDecision submit_decision(bool was_in_flight) {
    return was_in_flight ? SubmitDecision::kBusy : SubmitDecision::kProceed;
}

// 超时处理：任务尚未被游戏线程消费 -> 可取消；已被消费 -> 必须等待其完成。
enum class TimeoutAction {
    kCancelPending,
    kWaitForCompletion,
};
constexpr TimeoutAction on_timeout(bool consumed) {
    return consumed ? TimeoutAction::kWaitForCompletion : TimeoutAction::kCancelPending;
}

}  // namespace transition_detail

// 默认超时：普通状态切换（go_main_menu）。
constexpr int kTransitionTimeoutMs = 5000;
// 重转换（enter_slot / create_slot，含存档读取）超时。
constexpr int kTransitionHeavyTimeoutMs = 15000;
