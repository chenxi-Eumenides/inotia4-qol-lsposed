#include "infra/commands.h"

// 命令端口实现（见 infra/commands.h 的接口契约）。
//
// 槽位表固定 64 条，QolCommand 按值存放：命令体经 command_queue 在主线程执行时
// 从槽位读取命令，避免在 Command.ctx 上挂堆内存（跨边界无所有权问题）。
//
// 同步：一把互斥锁保护槽位表的分配与回收。选择锁而非 std::atomic<bool> CAS 的原因：
// 回收需要同时读票据终态并写回槽位字段（in_use/ticket），CAS 只能保护单字段，
// 多字段状态仍需额外同步；与 command_queue 采用同一把锁纪律，避免两套原语交错。
// 命令体（command_run）在锁外执行，因此 run 内调用 config/events 不会与 post 互锁；
// post 持槽位锁期间调用 command_queue::post，锁序恒为 slots → command_queue，
// 回收路径先释放 command_queue 锁再取槽位锁，不存在反向锁序。

#include <cstring>
#include <mutex>

#include "infra/command_queue.h"
#include "infra/config.h"

namespace qol::infra {
namespace {

constexpr uint32_t kCommandSlots = 64;

struct Slot {
    bool in_use = false;
    contracts::QolCommand command{};
    contracts::QolCommandTicket ticket = 0;
};

std::mutex g_slots_mtx;
Slot g_slots[kCommandSlots];

// 命令体：按 kind 分派。未知 kind 返回 false，让票据进入 kFailed，不静默成功。
// ctx 指向的槽位在票据进入终态前不会被回收（回收只在 commands_drain 内、run 之后）。
bool command_run(void* ctx) {
    const Slot* slot = static_cast<const Slot*>(ctx);
    switch (slot->command.kind) {
        case contracts::kCommandSetConfigBool:
            return config_set_bool(slot->command.key, slot->command.int_value != 0);
        default:
            return false;
    }
}

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

// 调用方需持有 g_slots_mtx。票据进终态（done/failed/cancelled）后槽位可复用。
void reap_terminal_slots_locked() {
    for (uint32_t i = 0; i < kCommandSlots; ++i) {
        Slot& slot = g_slots[i];
        if (!slot.in_use) {
            continue;
        }
        const CommandState state = ticket_state(slot.ticket);
        if (state == CommandState::kDone || state == CommandState::kFailed ||
            state == CommandState::kCancelled) {
            slot.in_use = false;
            slot.ticket = 0;
        }
    }
}

QolResult commands_post(contracts::QolCommandTicket* out,
                        const contracts::QolCommand* cmd) {
    if (out != nullptr) {
        *out = 0;
    }
    if (out == nullptr || cmd == nullptr) {
        return error_result(kQolErrInvalidArgument, "null argument");
    }
    if (cmd->key == nullptr) {
        return error_result(kQolErrInvalidArgument, "null key");
    }

    std::lock_guard<std::mutex> lock(g_slots_mtx);
    Slot* slot = nullptr;
    for (uint32_t i = 0; i < kCommandSlots; ++i) {
        if (!g_slots[i].in_use) {
            slot = &g_slots[i];
            break;
        }
    }
    if (slot == nullptr) {
        return error_result(kQolErrBlocked, "command slots full");
    }

    slot->command = *cmd;
    slot->in_use = true;
    const Command queued{"qol.command", command_run, slot, true};
    const CommandTicket ticket = post(queued);
    if (ticket == 0) {
        slot->in_use = false; // 入队失败，槽位立即归还
        return error_result(kQolErrBlocked, "command queue full");
    }
    slot->ticket = ticket;

    *out = ticket;
    return contracts::ok();
}

int32_t commands_state(contracts::QolCommandTicket ticket) {
    return static_cast<int32_t>(ticket_state(ticket));
}

QolResult commands_cancel(contracts::QolCommandTicket ticket) {
    if (cancel(ticket)) {
        return contracts::ok();
    }
    return error_result(kQolErrConflict, "ticket not cancellable");
}

}  // namespace

const contracts::QolCommands& commands_port() {
    static const contracts::QolCommands kPort{
        &commands_post,
        &commands_state,
        &commands_cancel,
    };
    return kPort;
}

uint32_t commands_drain() {
    const uint32_t executed = drain();
    std::lock_guard<std::mutex> lock(g_slots_mtx);
    reap_terminal_slots_locked();
    return executed;
}

void commands_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_slots_mtx);
    for (uint32_t i = 0; i < kCommandSlots; ++i) {
        g_slots[i].in_use = false;
        g_slots[i].ticket = 0;
    }
}

}  // namespace qol::infra
