#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../contracts/runtime_hooks.h"

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
// 步长不再来自 world_tuning 订阅：Host 的 better_move.step choice 直接写本地原子值。
std::atomic<int32_t> g_step{kDefaultStep};

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

namespace {

// 推送步长到游戏层；游戏层按值变化使 (模式, S) 候选缓存失效，插件侧只负责转发。
void push_step(int32_t step) {
    const auto* port = input_port();
    if (port == nullptr || port->api.set_step == nullptr) return;
    log_info("movement step=%d", step);
    port->api.set_step(step);
}

// 更新本地步长；值未变化时不重复推送。force 用于生命周期复位（安装/停用），
// 此时端口可能残留上一次激活的值，必须无条件写回原生 8。
void set_local_step(int32_t step, bool force) {
    const int32_t next = step < 1 ? kDefaultStep : step;
    const int32_t previous = g_step.exchange(next, std::memory_order_acq_rel);
    if (!force && previous == next) return;
    push_step(next);
}

}  // namespace

void reset_step() { set_local_step(kDefaultStep, true); }

void seed_default_step(const contracts::PluginContext& ctx) {
    // 只在插件已激活时回写 selection，避免 direction 关闭时仅因 step 选中而改变插件可用性。
    if (!gate_enabled()) return;
    const auto* api = contracts::feature_choice_if_available(ctx.sdk);
    if (api == nullptr || api->read_choice == nullptr || api->request_set_choice == nullptr) {
        // Host choice API 不可用：保持本地 8，不影响插件运行。
        return;
    }
    char selected[8]{};
    uint32_t size = 0;
    if (!contracts::succeeded(api->read_choice(api->context, ctx.owner_token, "better_move",
                                               "better_move.step", selected, sizeof(selected),
                                               &size)) ||
        size != 0) {
        // 已有 selection 或读取失败：不覆盖用户的显式选择。
        return;
    }
    // request_set_choice 是异步提交；持久化结果由 Host receipt 闭环，这里不等待也不重试。
    // Host 的 enqueue 要求 receipt 非空（否则直接返回 invalid argument），因此不能传 nullptr。
    contracts::FeatureReceipt receipt{};
    (void)api->request_set_choice(api->context, ctx.owner_token, "better_move",
                                  "better_move.step", "8", &receipt);
    log_info("movement step seeded default 8");
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

QolResult on_step_choice(contracts::PluginContext& ctx, const char* value) {
    // 空值表示未选择：本地按原生 8 生效；插件已激活时回写默认 "8" 保持持久化一致。
    // direction 关闭时 step 仍更新但不生效：模式由 direction 回调单独控制。
    if (value == nullptr || value[0] == '\0') {
        set_local_step(kDefaultStep, false);
        seed_default_step(ctx);
        return contracts::ok();
    }
    for (uint32_t i = 0; i < kStepChoiceCount; ++i) {
        if (std::strcmp(value, kStepChoices[i].value) != 0) continue;
        set_local_step(kStepAmounts[i], false);
        return contracts::ok();
    }
    return contracts::error(kQolErrInvalidArgument, nullptr, 0);
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
