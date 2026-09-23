#pragma once

// 主线程命令队列（ARCHITECTURE.md §6.1）：跨线程写游戏状态的唯一通道。
// 任意线程 post；游戏主线程每帧 drain 一次；执行结果写回票据供查询。
//
// 约束：命令体在游戏主线程执行，不得阻塞；票据状态可查询、可取消（仅限声明可取消
// 且尚未执行的命令）。

#include <cstdint>

namespace qol::infra {

using CommandTicket = uint64_t;  // 0 = 无效票据

enum class CommandState : int32_t {
    kPending = 0,
    kDone = 1,
    kFailed = 2,
    kCancelled = 3,
    kUnknown = 4,  // 票据不存在（id 从未分配）
    kExpired = 5,  // 状态记录已被有界窗口淘汰：结果不可判定，与 kFailed 区分
};

// ctx 所有权与生命周期由调用方保证：必须在票据进入终态后才可释放。
struct Command {
    const char* name = nullptr;   // 诊断用，静态字符串
    bool (*run)(void* ctx) = nullptr; // 返回 false = 执行失败（票据置 kFailed）
    void* ctx = nullptr;
    bool cancellable = false;
    // 可选：可写结果的命令体。非空时 run 应为 nullptr；drain 调用它并以返回值判定成功，
    // 结果 JSON 写入票据持有的有界缓冲（capacity 由队列提供，超限由命令体自行报失败）。
    // nullptr 表示该命令无结果（read_result 恒返回 false）。
    bool (*run_with_result)(void* ctx, char* out, uint32_t capacity, uint32_t* out_size) = nullptr;
    // 延迟结算上限（帧）。命令体可在执行期间调用 request_defer_current_command() 请求
    // 「保持 pending、下一帧重试」；超过上限仍未结算则票据置 failed 并写入超时错误体。
    // 0 表示不允许延迟（请求被忽略，按当帧结果结算）。
    uint32_t defer_limit_frames = 0;
};

// 当前正在执行的命令请求延迟结算：该票据保持 pending，drain 于后续帧重新执行本命令，
// 直到命令体自行结算或达到 defer_limit_frames。仅在命令执行期间（drain 内）有效。
void request_defer_current_command();

// 当前命令的执行次数（首次执行返回 0；每次延迟重试递增）。仅在命令执行期间有效，
// 供命令体区分「首次执行（需真正发起转换）」与「后续帧（仅等待就绪）」。
uint32_t current_command_attempt();

// 延迟结算默认上限（帧）≈2s@60fps：进档/建档等转场后的就绪等待。
constexpr uint32_t kDefaultDeferFrames = 120;

// 结果保留（明确尽力语义，见 ARCHITECTURE.md §8.1）：单结果上限与总字节预算有界；
// 超出总预算按最旧优先淘汰结果（保留状态）；被淘汰者 read_result 返回 false。
constexpr uint32_t kMaxResultBytes = 16384;
constexpr uint32_t kMaxRetainedResultBytes = 512 * 1024;

// 任意线程可调用；队列满或参数非法返回 0。
CommandTicket post(const Command& command);

// 未知票据返回 kUnknown；状态曾存在但已被窗口淘汰返回 kExpired。
CommandState ticket_state(CommandTicket ticket);

// 读取票据终态结果：成功时写入 out 并置 out_size（实际长度）；无结果、票据未知/过期、
// pending、或 capacity 不足均返回 false。明确尽力语义：结果可能已被预算淘汰。
bool read_result(CommandTicket ticket, char* out, uint32_t capacity, uint32_t* out_size);

// 仅在命令可取消且尚未执行时成功。
bool cancel(CommandTicket ticket);

// 主线程每帧调用；返回本次执行的命令条数。执行期间不得嵌套调用 drain。
uint32_t drain();

uint32_t pending_count();

// 测试用：清空队列与票据状态。
void command_queue_reset_for_test();

}  // namespace qol::infra
