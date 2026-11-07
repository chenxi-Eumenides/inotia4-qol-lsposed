#include "infra/event_state.h"

#include <array>
#include <cstddef>
#include <mutex>

namespace qol::infra {
namespace {

// 锁序：event_state mutex -> owner gate（owner_identity mutex）。owner gate 只做 token
// 校验，不得回调任何 event_state API；宿主 owner_suspend 在释放 owner mutex 后才执行
// 插件回调，因此不存在反向锁序。

using namespace contracts;

constexpr std::size_t kDeclarationCap = 32;
constexpr std::size_t kSubscriptionCap = 256;
constexpr std::size_t kQueueCap = 256;

struct State {
    bool used = false;
    QolOwnerToken owner = 0;  // 0 = 宿主 fallback 源，不是插件声明。
    uint16_t schema = 0;
    EventStateValue value{};
};

struct Subscription {
    bool used = false;
    uint64_t id = 0;
    uint32_t event = 0;
    QolEventStateHandler handler = nullptr;
    void* ctx = nullptr;
};

struct Pending {
    EventStateChange change{};
    QolOwnerToken owner = 0;
    uint64_t target_subscription = 0;  // 非 0：只投递给该订阅（订阅时快照）。
};

std::mutex g_mutex;
std::array<State, kDeclarationCap> g_states{};
std::array<Subscription, kSubscriptionCap> g_subscriptions{};
std::array<Pending, kQueueCap> g_queue{};
std::size_t g_head = 0;
std::size_t g_count = 0;
uint64_t g_next_id = 1;
QolResult (*g_verify_owner)(QolOwnerToken) = nullptr;
bool (*g_main_thread_gate)() = nullptr;

QolResult fail(int code) {
    return error(code, "event state", 11);
}

bool valid_event(uint32_t event_id) {
    return event_id == kEventStateSaveReady;
}

// token 0 is the host fallback source, not a plugin declaration.
bool owner_valid(QolOwnerToken owner) {
    if (owner == 0) return true;
    return g_verify_owner != nullptr && succeeded(g_verify_owner(owner));
}

bool declaration_active(QolOwnerToken owner) {
    return owner != 0 && owner_valid(owner);
}

State* find_state(uint32_t event_id) {
    for (State& state : g_states) {
        if (state.used && state.value.event_id == event_id) return &state;
    }
    return nullptr;
}

void enqueue(const EventStateChange& change, QolOwnerToken owner, uint64_t target_subscription = 0) {
    if (g_count == kQueueCap) {
        // 队列满：丢最旧项。状态本身已保存，后续订阅/查询仍能读到最新值。
        g_head = (g_head + 1) % kQueueCap;
        --g_count;
    }
    g_queue[(g_head + g_count) % kQueueCap] = Pending{change, owner, target_subscription};
    ++g_count;
}

EventStateChange make_change(const EventStateValue& value, EventStateChangeKind kind) {
    return EventStateChange{sizeof(EventStateChange),
                            value.event_id,
                            static_cast<uint8_t>(kind),
                            value.availability,
                            value.value,
                            0,
                            value.slot,
                            value.revision};
}

// 未发布 -> 首次发布：kSnapshot。已发布：只有 availability/value 真的变化才产生边沿。
EventStateChangeKind transition_kind(const EventStateValue& previous, uint8_t availability,
                                     uint8_t value) {
    if (previous.revision == 0) return EventStateChangeKind::kSnapshot;
    if (previous.availability != static_cast<uint8_t>(EventStateAvailability::kAvailable) &&
        availability == static_cast<uint8_t>(EventStateAvailability::kAvailable)) {
        return EventStateChangeKind::kSnapshot;  // owner 重新可用：按当前事实重新投递。
    }
    if (availability == static_cast<uint8_t>(EventStateAvailability::kUnavailable)) {
        return EventStateChangeKind::kUnavailable;
    }
    if (previous.value == 0 && value == 1) return EventStateChangeKind::kBecameReady;
    if (previous.value == 1 && value == 0) return EventStateChangeKind::kCeasedReady;
    return EventStateChangeKind::kSnapshot;
}

void write_state(State& state, uint8_t availability, uint8_t value, int32_t slot,
                 QolOwnerToken owner) {
    if (state.value.revision > 0 && state.value.availability == availability &&
        state.value.value == value) {
        return;  // 值不变：不递增 revision、不派发。
    }
    const EventStateChangeKind kind = transition_kind(state.value, availability, value);
    ++state.value.revision;
    state.value.availability = availability;
    state.value.value = value;
    state.value.slot = slot;
    enqueue(make_change(state.value, kind), owner);
}

}  // namespace

void event_state_set_owner_gate(QolResult (*verify)(QolOwnerToken)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_verify_owner = verify;
}

void event_state_set_main_thread_gate(bool (*gate)()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_main_thread_gate = gate;
}

QolResult event_state_declare(QolOwnerToken owner, const EventStateDescriptor& descriptor) {
    if (!valid_event(descriptor.event_id)) return fail(kQolErrNotFound);
    if (descriptor.size != sizeof(descriptor) ||
        descriptor.schema_version != kEventStateSaveReadySchema) {
        return fail(kQolErrUnsupported);
    }
    if (owner == 0 || !owner_valid(owner)) return fail(kQolErrBlocked);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (State* state = find_state(descriptor.event_id)) {
        if (state->owner == owner && state->schema == descriptor.schema_version) return ok();
        if (declaration_active(state->owner)) return fail(kQolErrConflict);
        // 旧声明失效或宿主 fallback：接管并重置为未发布。
        state->owner = owner;
        state->schema = descriptor.schema_version;
        state->value = EventStateValue{sizeof(EventStateValue), descriptor.event_id,
                                       descriptor.schema_version, 0, 0, -1, 0};
        return ok();
    }
    for (State& state : g_states) {
        if (state.used) continue;
        state.used = true;
        state.owner = owner;
        state.schema = descriptor.schema_version;
        state.value = EventStateValue{sizeof(EventStateValue), descriptor.event_id,
                                      descriptor.schema_version, 0, 0, -1, 0};
        return ok();
    }
    return fail(kQolErrBusy);
}

QolResult event_state_set(QolOwnerToken owner, const EventStateValue& value) {
    if (!valid_event(value.event_id)) return fail(kQolErrNotFound);
    if (value.size != sizeof(value) || value.schema_version != kEventStateSaveReadySchema) {
        return fail(kQolErrUnsupported);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_main_thread_gate != nullptr && !g_main_thread_gate()) return fail(kQolErrBlocked);
    State* state = find_state(value.event_id);
    if (state == nullptr || state->owner != owner) return fail(kQolErrBlocked);
    if (!owner_valid(state->owner)) {
        state->used = false;
        return fail(kQolErrBlocked);
    }
    write_state(*state, value.availability, value.value, value.slot, owner);
    return ok();
}

QolResult event_state_read(uint32_t event_id, EventStateValue* out_value) {
    if (!valid_event(event_id)) return fail(kQolErrNotFound);
    if (out_value == nullptr) return fail(kQolErrInvalidArgument);
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = find_state(event_id);
    if (state == nullptr || !owner_valid(state->owner) || state->value.revision == 0) {
        return fail(kQolErrNotReady);
    }
    *out_value = state->value;
    return ok();
}

QolResult event_state_subscribe(uint32_t event_id, QolEventStateHandler handler, void* ctx,
                                uint64_t* out_id) {
    if (!valid_event(event_id)) return fail(kQolErrNotFound);
    if (handler == nullptr || out_id == nullptr) return fail(kQolErrInvalidArgument);
    *out_id = 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    Subscription* target = nullptr;
    for (Subscription& subscription : g_subscriptions) {
        if (!subscription.used) {
            target = &subscription;
            break;
        }
    }
    if (target == nullptr) return fail(kQolErrBusy);

    const uint64_t id = g_next_id++;
    *target = Subscription{true, id, event_id, handler, ctx};
    *out_id = id;

    // 已发布才投递当前状态；未发布时订阅成功但不产生任何变更。
    if (State* state = find_state(event_id); state != nullptr && state->value.revision > 0) {
        enqueue(make_change(state->value, EventStateChangeKind::kSnapshot), state->owner, id);
    }
    return ok();
}

bool event_state_unsubscribe(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Subscription& subscription : g_subscriptions) {
        if (subscription.used && subscription.id == id) {
            subscription.used = false;
            return true;
        }
    }
    return false;
}

bool event_state_has_active_declaration(uint32_t event_id) {
    if (!valid_event(event_id)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = find_state(event_id);
    return state != nullptr && declaration_active(state->owner);
}

bool event_state_host_publish(uint32_t event_id, uint8_t availability, uint8_t value,
                              int32_t slot) {
    if (!valid_event(event_id)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = find_state(event_id);
    if (state != nullptr && declaration_active(state->owner)) return false;
    if (state == nullptr) {
        for (State& candidate : g_states) {
            if (!candidate.used) {
                state = &candidate;
                break;
            }
        }
    }
    if (state == nullptr) return false;
    if (state->owner != 0) {
        // 从失效的插件声明接管：丢弃其版本序列，按宿主来源重新开始。
        *state = State{true, 0, kEventStateSaveReadySchema,
                       EventStateValue{sizeof(EventStateValue), event_id,
                                       kEventStateSaveReadySchema, 0, 0, -1, 0}};
    } else {
        state->used = true;
        state->schema = kEventStateSaveReadySchema;
        state->value.event_id = event_id;
        state->value.schema_version = kEventStateSaveReadySchema;
    }
    write_state(*state, availability, value, slot, 0);
    return true;
}

uint32_t event_state_dispatch_pending() {
    // 先清理已失效 owner 的声明：即使停用回调里的 set_state 失败（token 已被宿主提前
    // suspend），订阅者也必须收到一次 kUnavailable，而不是静默残留旧值。
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (State& state : g_states) {
            if (!state.used || state.owner == 0 || owner_valid(state.owner)) continue;
            if (state.value.revision > 0) {
                state.value.availability = 0;
                state.value.value = 0;
                state.value.slot = -1;
                ++state.value.revision;
                enqueue(make_change(state.value, EventStateChangeKind::kUnavailable), 0);
            }
            state.used = false;
        }
    }

    std::array<Pending, kQueueCap> items{};
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        while (g_count != 0) {
            items[count++] = g_queue[g_head];
            g_head = (g_head + 1) % kQueueCap;
            --g_count;
        }
    }

    uint32_t calls = 0;
    std::array<uint32_t, kQueueCap> invalidated{};
    std::size_t invalidated_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Pending pending = items[i];
        if (pending.owner != 0) {
            // 生产 owner 已失效：不调用其回调，改写为一次 unavailable 并清理声明。
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!owner_valid(pending.owner)) {
                bool seen = false;
                for (std::size_t n = 0; n < invalidated_count; ++n) {
                    if (invalidated[n] == pending.change.event_id) seen = true;
                }
                if (seen) continue;
                invalidated[invalidated_count++] = pending.change.event_id;
                State* state = find_state(pending.change.event_id);
                if (state == nullptr || state->owner != pending.owner) continue;
                if (state->value.revision > 0) {
                    state->value.availability = 0;
                    state->value.value = 0;
                    state->value.slot = -1;
                    ++state->value.revision;
                    pending.change =
                        make_change(state->value, EventStateChangeKind::kUnavailable);
                    pending.target_subscription = 0;
                }
                state->used = false;
            }
        }

        std::array<Subscription, kSubscriptionCap> targets{};
        std::size_t target_count = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const Subscription& subscription : g_subscriptions) {
                if (!subscription.used || subscription.event != pending.change.event_id) continue;
                if (pending.target_subscription != 0 &&
                    subscription.id != pending.target_subscription) {
                    continue;
                }
                targets[target_count++] = subscription;
            }
        }
        // 锁外回调：handler 可以再次 subscribe/unsubscribe/set_state。
        for (std::size_t n = 0; n < target_count; ++n) {
            targets[n].handler(targets[n].ctx, &pending.change);
            ++calls;
        }
    }
    return calls;
}

void event_state_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_states = {};
    g_subscriptions = {};
    g_queue = {};
    g_head = 0;
    g_count = 0;
    g_next_id = 1;
    g_verify_owner = nullptr;
    g_main_thread_gate = nullptr;
}

}  // namespace qol::infra
