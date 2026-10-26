#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../contracts/runtime_hooks.h"

namespace qol::features::eight_way_move {
namespace {

const contracts::QolHostSdk* g_sdk = nullptr;
std::atomic<const contracts::QolMovementInputPort*> g_input_port{nullptr};
std::atomic<uint64_t> g_poll_task{0};
std::atomic<bool> g_gate{false};
std::atomic<void*> g_backup_direction{nullptr};
std::atomic<void*> g_backup_press_key{nullptr};
std::atomic<bool> g_direction_seen{false};
std::atomic<bool> g_move_seen{false};

using DirectionFn = uint64_t (*)(void*, uint64_t, int64_t*, uint64_t);
using PressKeyFn = void (*)(int32_t);

void log_info(const char* format, ...) {
    const auto* sdk = g_sdk;
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0) return;
    const uint32_t message_length = static_cast<uint32_t>(
        length < static_cast<int>(contracts::kMessageCap) ? length : contracts::kMessageCap);
    (void)sdk->log.write(1, 0, "plugins/eight_way_move/feature.cpp", message, message_length);
}

}  // namespace

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

void set_host_sdk(const contracts::QolHostSdk* sdk) { g_sdk = sdk; }

const contracts::QolMovementInputPort* input_port() {
    return g_input_port.load(std::memory_order_acquire);
}

void set_input_port(const contracts::QolMovementInputPort* port) {
    g_input_port.store(port, std::memory_order_release);
}

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
}

bool gate_enabled() {
    return g_gate.load(std::memory_order_acquire);
}

int32_t state_poll_tick(int64_t /*frame*/, void* /*ctx*/) {
    const auto* port = input_port();
    if (port != nullptr && port->api.poll != nullptr) port->api.poll();
    return 1;
}

void ensure_poll_task() {
    if (g_poll_task.load(std::memory_order_acquire) != 0) return;
    const auto* sdk = g_sdk;
    if (sdk == nullptr || sdk->scheduler.add == nullptr) return;
    uint64_t id = 0;
    const QolResult result = sdk->scheduler.add(
        contracts::kQolRuntimeFramePointLogicPre, &state_poll_tick, nullptr, 30, 0, &id);
    if (contracts::succeeded(result) && id != 0) {
        g_poll_task.store(id, std::memory_order_release);
    }
}

void remove_poll_task() {
    const uint64_t id = g_poll_task.exchange(0, std::memory_order_acq_rel);
    const auto* sdk = g_sdk;
    if (id != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(id);
    }
}

uint64_t direction_touch_wrapper(void* ctrl, uint64_t event, int64_t* point,
                                 uint64_t param3) {
    const auto backup = reinterpret_cast<DirectionFn>(
        g_backup_direction.load(std::memory_order_acquire));
    if (backup == nullptr) return 0;
    const uint64_t result = backup(ctrl, event, point, param3);
    const auto* port = input_port();
    if (gate_enabled() && port != nullptr && port->api.on_direction_touch != nullptr) {
        port->api.on_direction_touch(ctrl, event, point, param3);
        if (!g_direction_seen.exchange(true, std::memory_order_acq_rel)) {
            log_info("direction hook active");
        }
    }
    return result;
}

void press_key_play_wrapper(int32_t key) {
    const auto backup = reinterpret_cast<PressKeyFn>(
        g_backup_press_key.load(std::memory_order_acquire));
    if (backup == nullptr) return;
    int32_t mapped = key;
    const auto* port = input_port();
    if (gate_enabled() && port != nullptr && port->api.translate_press_key != nullptr) {
        mapped = port->api.translate_press_key(key);
    }
    backup(mapped);
}

int32_t eight_way_char_move_wrapper(void* ch, int32_t dir, int32_t delta, uint8_t flag) {
    const auto* port = input_port();
    if (port == nullptr || port->api.char_move == nullptr) return 0;
    if (!g_move_seen.exchange(true, std::memory_order_acq_rel)) {
        log_info("char_move hook active");
    }
    // 端口内部按 enabled 门禁决定透传原 CHAR_Move 或双轴移动。
    return port->api.char_move(ch, dir, delta, flag);
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdDirectionTouch) == 0) {
        g_backup_direction.store(backup, std::memory_order_release);
    } else if (std::strcmp(resource_id, kResourceIdPressKeyPlay) == 0) {
        g_backup_press_key.store(backup, std::memory_order_release);
    }
    return 0;
}

}  // namespace qol::features::eight_way_move
