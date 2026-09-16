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
};

// ctx 所有权与生命周期由调用方保证：必须在票据进入终态后才可释放。
struct Command {
    const char* name = nullptr;   // 诊断用，静态字符串
    bool (*run)(void* ctx) = nullptr; // 返回 false = 执行失败（票据置 kFailed）
    void* ctx = nullptr;
    bool cancellable = false;
};

// 任意线程可调用；队列满或参数非法返回 0。
CommandTicket post(const Command& command);

// 未知票据返回 kFailed。
CommandState ticket_state(CommandTicket ticket);

// 仅在命令可取消且尚未执行时成功。
bool cancel(CommandTicket ticket);

// 主线程每帧调用；返回本次执行的命令条数。执行期间不得嵌套调用 drain。
uint32_t drain();

uint32_t pending_count();

// 测试用：清空队列与票据状态。
void command_queue_reset_for_test();

}  // namespace qol::infra
