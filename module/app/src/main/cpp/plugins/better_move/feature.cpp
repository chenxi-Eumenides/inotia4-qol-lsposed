#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../contracts/runtime_hooks.h"
#include "../../contracts/world_tuning.h"

namespace qol::features::better_move {
namespace {

const contracts::QolHostSdk* g_sdk = nullptr;
std::atomic<const contracts::QolMovementInputPort*> g_input_port{nullptr};
std::atomic<uint64_t> g_poll_task{0};
std::atomic<bool> g_gate{false};
std::atomic<void*> g_backup_direction{nullptr};
std::atomic<void*> g_backup_press_key{nullptr};
std::atomic<void*> g_backup_porting_draw{nullptr};
std::atomic<void*> g_backup_wait_coord{nullptr};
std::atomic<void*> g_backup_map_blocking{nullptr};
std::atomic<bool> g_direction_seen{false};
std::atomic<bool> g_move_seen{false};
std::atomic<bool> g_wait_coord_seen{false};
std::atomic<bool> g_map_blocking_seen{false};

const contracts::WorldTuningMoveSpeedApi* g_move_speed_api = nullptr;
int32_t g_pushed_step = contracts::kWorldTuningBaseMoveSpeed;
uint32_t g_step_refresh_ticks = 0;

using DirectionFn = uint64_t (*)(void*, uint64_t, int64_t*, uint64_t);
using PressKeyFn = void (*)(int32_t);
using PortingDrawFn = void (*)();
using WaitCoordFn = void (*)(void*);
using MapBlockingFn = int32_t (*)(int32_t, int32_t);

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
    (void)sdk->log.write(1, 0, "plugins/better_move/feature.cpp", message, message_length);
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

void sync_move_speed(bool force) {
    const auto* port = input_port();
    if (port == nullptr || port->api.set_step == nullptr) return;

    const auto* sdk = g_sdk;
    if (force || ++g_step_refresh_ticks >= 30) {
        g_step_refresh_ticks = 0;
        const void* iface = nullptr;
        const bool available =
            sdk != nullptr && sdk->capabilities.require != nullptr &&
            contracts::succeeded(sdk->capabilities.require(
                contracts::kWorldTuningMoveSpeedCapabilityId,
                {contracts::kWorldTuningMoveSpeedVersion.major,
                 contracts::kWorldTuningMoveSpeedVersion.minor},
                &iface)) &&
            iface != nullptr;
        if (available) {
            const auto* api = static_cast<const contracts::WorldTuningMoveSpeedApi*>(iface);
            g_move_speed_api = api->read != nullptr ? api : nullptr;
        } else {
            g_move_speed_api = nullptr;
        }
    }

    int32_t step = contracts::kWorldTuningBaseMoveSpeed;
    if (g_move_speed_api != nullptr) {
        int32_t current = 0;
        if (contracts::succeeded(g_move_speed_api->read(&current)) && current >= 1) {
            step = current;
        } else {
            g_move_speed_api = nullptr;
        }
    }
    if (step != g_pushed_step) {
        g_pushed_step = step;
        log_info("move speed step=%d", step);
        port->api.set_step(step);
    }
}

void reset_move_speed() {
    g_move_speed_api = nullptr;
    g_step_refresh_ticks = 0;
    const int32_t previous = g_pushed_step;
    g_pushed_step = contracts::kWorldTuningBaseMoveSpeed;
    const auto* port = input_port();
    if (port != nullptr && port->api.set_step != nullptr &&
        previous != contracts::kWorldTuningBaseMoveSpeed) {
        port->api.set_step(contracts::kWorldTuningBaseMoveSpeed);
    }
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
    sync_move_speed(false);
    return 1;
}

void ensure_poll_task() {
    if (g_poll_task.load(std::memory_order_acquire) != 0) return;
    const auto* sdk = g_sdk;
    if (sdk == nullptr || sdk->scheduler.add == nullptr) return;
    uint64_t id = 0;
    const QolResult result = sdk->scheduler.add(
        contracts::kQolRuntimeFramePointLogicPre, &state_poll_tick, nullptr, 1, 0, &id);
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

void porting_draw_wrapper() {
    const auto backup = reinterpret_cast<PortingDrawFn>(
        g_backup_porting_draw.load(std::memory_order_acquire));
    if (backup == nullptr) return;
    const auto* port = input_port();
    if (gate_enabled() && port != nullptr && port->api.prepare_overlay != nullptr) {
        port->api.prepare_overlay();
    }
    backup();
    if (gate_enabled() && port != nullptr && port->api.draw_overlay != nullptr) {
        port->api.draw_overlay();
    }
}

int32_t char_move_wrapper(void* ch, int32_t dir, int32_t delta, uint8_t flag) {
    const auto* port = input_port();
    if (port == nullptr || port->api.char_move == nullptr) return 0;
    if (!g_move_seen.exchange(true, std::memory_order_acq_rel)) {
        log_info("char_move hook active");
    }
    // 端口内部按 enabled 门禁决定透传原 CHAR_Move 或全向双轴移动。
    return port->api.char_move(ch, dir, delta, flag);
}

void move_on_wait_coord_wrapper(void* ch) {
    const auto backup =
        reinterpret_cast<WaitCoordFn>(g_backup_wait_coord.load(std::memory_order_acquire));
    if (backup == nullptr) return;
    const auto* port = input_port();
    if (gate_enabled() && port != nullptr && port->api.handle_wait_coord != nullptr &&
        port->api.handle_wait_coord(ch) != 0) {
        if (!g_wait_coord_seen.exchange(true, std::memory_order_acq_rel)) {
            log_info("wait coord hook active");
        }
        return;
    }
    backup(ch);
}

int32_t map_blocking_wrapper(int32_t x, int32_t y) {
    const auto backup =
        reinterpret_cast<MapBlockingFn>(g_backup_map_blocking.load(std::memory_order_acquire));
    if (backup == nullptr) return 1;
    const auto* port = input_port();
    if (gate_enabled() && port != nullptr && port->api.map_blocking_query != nullptr) {
        if (!g_map_blocking_seen.exchange(true, std::memory_order_acq_rel)) {
            log_info("map blocking hook active");
        }
        return port->api.map_blocking_query(x, y, backup);
    }
    return backup(x, y);
}

QolResult on_choice(contracts::PluginContext& ctx, const char* value) {
    (void)ctx;
    const auto* port = input_port();
    if (!contracts::movement_input_port_supports_mode(port))
        return contracts::error(kQolErrNotReady, nullptr, 0);

    contracts::QolMovementInputMode mode = contracts::QolMovementInputMode::kDisabled;
    if (value != nullptr && value[0] != '\0') {
        if (std::strcmp(value, "eight_way") == 0) {
            mode = contracts::QolMovementInputMode::kEightWay;
        } else if (std::strcmp(value, "omnidirectional") == 0) {
            mode = contracts::QolMovementInputMode::kOmnidirectional;
        } else {
            return contracts::error(kQolErrInvalidArgument, nullptr, 0);
        }
    }
    port->api.set_mode(mode);
    return contracts::ok();
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdDirectionTouch) == 0) {
        g_backup_direction.store(backup, std::memory_order_release);
    } else if (std::strcmp(resource_id, kResourceIdPressKeyPlay) == 0) {
        g_backup_press_key.store(backup, std::memory_order_release);
    } else if (std::strcmp(resource_id, kResourceIdPortingDraw) == 0) {
        g_backup_porting_draw.store(backup, std::memory_order_release);
    } else if (std::strcmp(resource_id, kResourceIdWaitCoord) == 0) {
        g_backup_wait_coord.store(backup, std::memory_order_release);
    } else if (std::strcmp(resource_id, kResourceIdMapBlocking) == 0) {
        g_backup_map_blocking.store(backup, std::memory_order_release);
    }
    return 0;
}

}  // namespace qol::features::better_move
