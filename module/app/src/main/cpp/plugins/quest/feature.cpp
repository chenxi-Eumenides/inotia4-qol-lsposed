#include "plugins/quest/feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace qol::features::quest {
namespace {

std::atomic<bool> g_gate{false};
uint64_t g_resync_task = 0;
bool g_was_in_world = false;

void log_write(int32_t level, const char* format, va_list args) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    if (length < 0) return;
    const uint32_t message_length = static_cast<uint32_t>(
        length < static_cast<int>(contracts::kMessageCap) ? length : contracts::kMessageCap);
    (void)sdk->log.write(level, 0, "plugins/quest/feature.cpp", message, message_length);
}

void log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(1, format, args);
    va_end(args);
}

int32_t resync_once() {
    const auto* game = game_host();
    if (game == nullptr || game->extra.quest.active_slot_count == nullptr ||
        game->extra.quest.active_slot_id == nullptr || game->extra.quest.state == nullptr ||
        game->extra.quest.is_complete == nullptr || game->extra.quest_write.set_state == nullptr) {
        return 0;
    }

    uint32_t fixed = 0;
    uint32_t count = 0;
    if (game->extra.quest.active_slot_count(&count).ok != kQolOk) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t quest_id = 0;
        if (game->extra.quest.active_slot_id(i, &quest_id).ok != kQolOk) continue;
        int32_t state = 0;
        if (game->extra.quest.state(quest_id, &state).ok != kQolOk || state != 1) continue;
        int32_t complete = 0;
        if (game->extra.quest.is_complete(quest_id, &complete).ok != kQolOk || complete == 0) {
            continue;
        }
        if (game->extra.quest_write.set_state(quest_id, 2).ok == kQolOk) ++fixed;
    }
    log_info("quest resync: fixed=%u", fixed);
    return 0;
}

int32_t world_transition_tick(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    if (!g_gate.load(std::memory_order_acquire)) return 1;
    const auto* game = game_host();
    if (game == nullptr || game->read.world.state == nullptr) return 1;
    int32_t state = 0;
    if (game->read.world.state(&state).ok != kQolOk) return 1;
    const bool in_world = state == 5;
    if (!in_world) {
        g_was_in_world = false;
        return 1;
    }
    if (g_was_in_world) return 1;
    g_was_in_world = true;
    (void)resync_tick(frame, ctx);
    return 1;
}

}  // namespace

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
}

bool gate_enabled() {
    return g_gate.load(std::memory_order_acquire);
}

void cancel_resync_task() {
    const uint64_t task = g_resync_task;
    g_resync_task = 0;
    g_was_in_world = false;
    const auto* sdk = host_sdk();
    if (task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(task);
    }
}

void start_resync_task() {
    if (g_resync_task != 0) return;
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->scheduler.add == nullptr) return;
    uint64_t task = 0;
    if (sdk->scheduler.add(1, &world_transition_tick, nullptr, 1, 0, &task).ok == kQolOk) {
        g_resync_task = task;
    }
}

void on_save_enter(void* ctx) {
    (void)ctx;
    g_was_in_world = false;
    start_resync_task();
}

int32_t resync_tick(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    if (!g_gate.load(std::memory_order_acquire)) return 0;
    return resync_once();
}

}  // namespace qol::features::quest
