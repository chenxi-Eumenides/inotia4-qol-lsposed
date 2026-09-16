// infra/command_queue.h 实现（纯 STL，不依赖 Android）。
// 跨线程 post 与主线程 drain 通过一把互斥锁协调；命令体一律在锁外执行，
// 因此 post 的调用方（任意线程）与 drain 的主线程都不会因命令体阻塞而死锁。

#include "infra/command_queue.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace qol::infra {
namespace {

// 待执行队列容量上限：满则 post 返回 0。命令体由调用方保证不阻塞，正常一帧内即可
// 排空，1024 足够容纳一帧的突发；不设上限会让异常调用方无限堆积内存。
constexpr std::size_t kMaxPending = 1024;

// 终态票据保留上限：只记最近 1024 条 kDone/kFailed/kCancelled。
// 原因：票据 id 单调递增且不会回收，若终态表无界增长会随运行时间无限膨胀；
// 调用方约定在命令进入终态后及时查询，1024 条的窗口足够覆盖正常轮询间隔。
// 被淘汰的旧票据视为未知，ticket_state 返回 kFailed（与「未知票据」同语义）。
constexpr std::size_t kMaxTerminalStates = 1024;

struct ActiveEntry {
    Command command;
    bool cancellable = false;
    bool started = false;  // drain 已开始执行；已开始的命令不再可取消
};

std::mutex g_mtx;
// 活跃票据（pending + 执行中）与其 FIFO 顺序。pending_count 只数 FIFO 中尚未开始者。
std::unordered_map<CommandTicket, ActiveEntry> g_active;
std::deque<CommandTicket> g_fifo;
// 终态票据（有界，见 kMaxTerminalStates）。
std::unordered_map<CommandTicket, CommandState> g_terminal;
std::deque<CommandTicket> g_terminal_order;
CommandTicket g_next_ticket = 1;  // 票据从 1 起单调递增

void record_terminal_locked(CommandTicket ticket, CommandState state) {
    g_terminal[ticket] = state;
    g_terminal_order.push_back(ticket);
    if (g_terminal_order.size() > kMaxTerminalStates) {
        const CommandTicket oldest = g_terminal_order.front();
        g_terminal_order.pop_front();
        g_terminal.erase(oldest);
    }
}

}  // namespace

CommandTicket post(const Command& command) {
    if (command.run == nullptr || command.name == nullptr) return 0;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_fifo.size() >= kMaxPending) return 0;
    const CommandTicket ticket = g_next_ticket;
    ++g_next_ticket;
    g_active.emplace(ticket, ActiveEntry{command, command.cancellable, false});
    g_fifo.push_back(ticket);
    return ticket;
}

CommandState ticket_state(CommandTicket ticket) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const auto it = g_active.find(ticket);
    if (it != g_active.end()) return CommandState::kPending;  // started 与否都仍是 kPending
    const auto term = g_terminal.find(ticket);
    if (term != g_terminal.end()) return term->second;
    return CommandState::kFailed;  // 未知票据，或已被有界窗口淘汰
}

bool cancel(CommandTicket ticket) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const auto it = g_active.find(ticket);
    if (it == g_active.end()) return false;              // 未知、已终态或正在执行
    if (!it->second.cancellable || it->second.started) return false;
    g_active.erase(it);
    const auto fifo_it = std::find(g_fifo.begin(), g_fifo.end(), ticket);
    if (fifo_it != g_fifo.end()) g_fifo.erase(fifo_it);
    record_terminal_locked(ticket, CommandState::kCancelled);
    return true;
}

uint32_t drain() {
    // 先取快照：本帧只执行进入 drain 时已 pending 的命令；执行期间新 post 的留到下一帧。
    // 快照后立即从 active 中移除并置 started，使 cancel 无法再命中本批命令。
    std::vector<std::pair<CommandTicket, Command>> batch;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        batch.reserve(g_fifo.size());
        for (const CommandTicket ticket : g_fifo) {
            ActiveEntry& entry = g_active[ticket];
            entry.started = true;  // 标记已开始：执行中取消一律失败
            batch.emplace_back(ticket, entry.command);
        }
        g_fifo.clear();
    }

    // 锁外逐条执行：命令体是外部代码，持锁回调会阻塞 post 并可能造成死锁。
    for (auto& item : batch) {
        const CommandTicket ticket = item.first;
        const Command& command = item.second;
        const bool ok = command.run(command.ctx);
        std::lock_guard<std::mutex> lock(g_mtx);
        g_active.erase(ticket);
        record_terminal_locked(ticket, ok ? CommandState::kDone : CommandState::kFailed);
    }
    return static_cast<uint32_t>(batch.size());
}

uint32_t pending_count() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return static_cast<uint32_t>(g_fifo.size());
}

void command_queue_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_active.clear();
    g_fifo.clear();
    g_terminal.clear();
    g_terminal_order.clear();
    g_next_ticket = 1;
}

}  // namespace qol::infra
