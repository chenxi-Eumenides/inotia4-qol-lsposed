// infra/command_queue.h 实现（纯 STL，不依赖 Android）。
// 跨线程 post 与主线程 drain 通过一把互斥锁协调；命令体一律在锁外执行，
// 因此 post 的调用方（任意线程）与 drain 的主线程都不会因命令体阻塞而死锁。

#include "infra/command_queue.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
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
    uint32_t attempts = 0; // 延迟结算重试次数（见 Command.defer_limit_frames）
};

std::mutex g_mtx;

// 当前执行中的命令的延迟结算请求（仅主线程 drain 期间有意义）。
thread_local bool g_defer_requested = false;
thread_local uint32_t g_current_attempt = 0;
// 活跃票据（pending + 执行中）与其 FIFO 顺序。pending_count 只数 FIFO 中尚未开始者。
std::unordered_map<CommandTicket, ActiveEntry> g_active;
std::deque<CommandTicket> g_fifo;
// 终态票据（有界，见 kMaxTerminalStates）与其结果（另受 kMaxRetainedResultBytes 预算约束）。
struct TerminalEntry {
    CommandState state = CommandState::kDone;
    std::string result;  // 空 = 无结果或结果已被预算淘汰
};
std::unordered_map<CommandTicket, TerminalEntry> g_terminal;
std::deque<CommandTicket> g_terminal_order;
std::size_t g_retained_result_bytes = 0;
CommandTicket g_next_ticket = 1;  // 票据从 1 起单调递增

void record_terminal_locked(CommandTicket ticket, CommandState state, const char* result,
                            uint32_t result_size) {
    TerminalEntry entry;
    entry.state = state;
    if (result != nullptr && result_size != 0 && result_size <= kMaxResultBytes) {
        entry.result.assign(result, result_size);
    }
    g_retained_result_bytes += entry.result.size();
    g_terminal[ticket] = std::move(entry);
    g_terminal_order.push_back(ticket);
    if (g_terminal_order.size() > kMaxTerminalStates) {
        const CommandTicket oldest = g_terminal_order.front();
        g_terminal_order.pop_front();
        const auto it = g_terminal.find(oldest);
        if (it != g_terminal.end()) {
            g_retained_result_bytes -= it->second.result.size();
            g_terminal.erase(it);
        }
    }
    // 结果预算：超预算时从最旧开始丢弃结果文本（保留状态），保证明确尽力语义有界。
    for (const CommandTicket t : g_terminal_order) {
        if (g_retained_result_bytes <= kMaxRetainedResultBytes) break;
        const auto it = g_terminal.find(t);
        if (it != g_terminal.end() && !it->second.result.empty()) {
            g_retained_result_bytes -= it->second.result.size();
            it->second.result.clear();
        }
    }
}

}  // namespace

CommandTicket post(const Command& command) {
    if (command.name == nullptr) return 0;
    if (command.run == nullptr && command.run_with_result == nullptr) return 0;
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
    if (ticket == 0) return CommandState::kUnknown;
    const auto it = g_active.find(ticket);
    if (it != g_active.end()) return CommandState::kPending;  // started 与否都仍是 kPending
    const auto term = g_terminal.find(ticket);
    if (term != g_terminal.end()) return term->second.state;
    // 未命中：id 小于已分配上界说明曾分配但状态已被窗口淘汰（kExpired）；否则从未分配（kUnknown）。
    return ticket < g_next_ticket ? CommandState::kExpired : CommandState::kUnknown;
}

bool read_result(CommandTicket ticket, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mtx);
    const auto it = g_terminal.find(ticket);
    if (it == g_terminal.end()) return false;
    const std::string& result = it->second.result;
    if (result.empty() || result.size() > capacity) return false;
    std::memcpy(out, result.data(), result.size());
    *out_size = static_cast<uint32_t>(result.size());
    return true;
}

bool cancel(CommandTicket ticket) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const auto it = g_active.find(ticket);
    if (it == g_active.end()) return false;              // 未知、已终态或正在执行
    if (!it->second.cancellable || it->second.started) return false;
    g_active.erase(it);
    const auto fifo_it = std::find(g_fifo.begin(), g_fifo.end(), ticket);
    if (fifo_it != g_fifo.end()) g_fifo.erase(fifo_it);
    record_terminal_locked(ticket, CommandState::kCancelled, nullptr, 0);
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
    static thread_local std::vector<char> result_buffer(kMaxResultBytes);
    uint32_t executed = 0;
    for (auto& item : batch) {
        const CommandTicket ticket = item.first;
        const Command& command = item.second;
        g_defer_requested = false;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            const auto it = g_active.find(ticket);
            g_current_attempt = it != g_active.end() ? it->second.attempts : 0;
        }
        bool ok = false;
        uint32_t result_size = 0;
        if (command.run_with_result != nullptr) {
            ok = command.run_with_result(command.ctx, result_buffer.data(),
                                         static_cast<uint32_t>(result_buffer.size()), &result_size);
        } else if (command.run != nullptr) {
            ok = command.run(command.ctx);
        }
        const bool defer = g_defer_requested;
        g_defer_requested = false;
        std::lock_guard<std::mutex> lock(g_mtx);
        const auto it = g_active.find(ticket);
        if (it == g_active.end()) continue;  // 已取消（started=true 后 cancel 恒失败，理论不可达）
        ++it->second.attempts;
        if (defer && command.defer_limit_frames > 0) {
            if (it->second.attempts < command.defer_limit_frames) {
                // 保持 pending，下一帧重试；重试窗口内回到可取消态。
                it->second.started = false;
                g_fifo.push_back(ticket);
                ++executed;
                continue;
            }
            // 达到延迟上限仍未结算：显式失败，不静默成功。
            g_active.erase(it);
            static const char kTimeout[] = "{\"ok\":false,\"error\":\"settlement timeout\"}";
            record_terminal_locked(ticket, CommandState::kFailed, kTimeout,
                                   static_cast<uint32_t>(sizeof(kTimeout) - 1));
            ++executed;
            continue;
        }
        g_active.erase(it);
        record_terminal_locked(ticket, ok ? CommandState::kDone : CommandState::kFailed,
                               result_size != 0 ? result_buffer.data() : nullptr, result_size);
        ++executed;
    }
    return executed;
}

void request_defer_current_command() {
    g_defer_requested = true;
}

uint32_t current_command_attempt() {
    return g_current_attempt;
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
    g_retained_result_bytes = 0;
    g_next_ticket = 1;
}

}  // namespace qol::infra
