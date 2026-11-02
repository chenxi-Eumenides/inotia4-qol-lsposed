#include "host/owner_identity.h"

#include <array>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace qol::host {
namespace {
struct Owner {
    void* identity = nullptr;
    std::string plugin_id;
    contracts::QolOwnerToken token = 0;
    uint64_t generation = 1;
    uint64_t previous_token = 0;
    std::string store_namespace;
    bool slot_operation_allowed = false;
    bool save_route_allowed = false;
    std::size_t namespace_calls = 0;
    bool suspending = false;
    bool finishing_suspend = false;
    bool stopped_notified = false;
    bool route_notified = false;
    uint64_t pending_token = 0;
    bool finishing_resume = false;
};
std::array<Owner, 32> g_owners;
std::mutex g_mutex;
std::condition_variable g_condition;
uint64_t g_next_token = 1;
void (*g_store_stopped)(uint64_t) = nullptr;
void (*g_store_rebound)(uint64_t, uint64_t) = nullptr;
void (*g_route_stopped)(uint64_t) = nullptr;
bool (*g_store_stopped_try)(uint64_t) = nullptr;
bool (*g_store_rebound_try)(uint64_t, uint64_t) = nullptr;
bool (*g_route_stopped_try)(uint64_t) = nullptr;

QolResult fail(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}
Owner* find_identity(void* identity) {
    for (auto& owner : g_owners) if (owner.identity == identity) return &owner;
    return nullptr;
}
Owner* find_token(contracts::QolOwnerToken token) {
    if (token == 0) return nullptr;
    for (auto& owner : g_owners) if (owner.identity != nullptr && owner.token == token) return &owner;
    return nullptr;
}
bool issue(Owner& owner) {
    if (owner.token != 0) return true;
    if (owner.suspending || owner.namespace_calls != 0) return false;
    if (g_next_token == std::numeric_limits<uint64_t>::max()) return false;
    owner.token = g_next_token++;
    return true;
}
QolResult query(void* identity, contracts::QolOwnerToken* token, uint64_t* generation) {
    if (identity == nullptr || token == nullptr || generation == nullptr)
        return fail(kQolErrInvalidArgument, "invalid owner query");
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = find_identity(identity);
    if (owner == nullptr) return fail(kQolErrNotFound, "owner not registered");
    *token = owner->token;
    *generation = owner->generation;
    return contracts::ok();
}
QolResult verify(void* identity, contracts::QolOwnerToken token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = find_token(token);
    if (owner == nullptr || (identity != nullptr && owner->identity != identity))
        return fail(kQolErrBlocked, "owner token invalid");
    return contracts::ok();
}
const contracts::QolOwnerIdentityApi kApi{{sizeof(contracts::QolOwnerIdentityApi), 1, 0},
                                       &query, &verify};
}  // namespace

bool owner_register(void* identity, const char* plugin_id) {
    if (identity == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto* existing = find_identity(identity); existing != nullptr) {
        if (plugin_id == nullptr) return true;
        if (!existing->plugin_id.empty() && existing->plugin_id != plugin_id) return false;
        existing->plugin_id = plugin_id;
        return true;
    }
    for (auto& owner : g_owners) {
        if (owner.identity != nullptr) continue;
        owner.identity = identity;
        if (plugin_id != nullptr) owner.plugin_id = plugin_id;
        if (issue(owner)) return true;
        owner = Owner{};
        return false;
    }
    return false;
}
bool owner_resume(void* identity) {
    void (*rebound)(uint64_t, uint64_t) = nullptr;
    uint64_t previous = 0;
    uint64_t current = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto* owner = identity == nullptr ? nullptr : find_identity(identity);
        if (!owner) return false;
        if (owner->pending_token || owner->finishing_resume) return false;
        const bool fresh = owner->token == 0;
        if (!issue(*owner)) return false;
        if (fresh && owner->previous_token != 0 &&
            (!owner->store_namespace.empty() || owner->slot_operation_allowed)) {
            rebound = g_store_rebound;
            previous = owner->previous_token;
            current = owner->token;
        }
    }
    if (rebound) rebound(previous, current);
    return true;
}
bool owner_resume_poll(void* identity) {
    std::unique_lock<std::mutex> lock(g_mutex);
    auto* owner = identity ? find_identity(identity) : nullptr;
    if (!owner) return false;
    if (owner->token) return true;
    if (owner->suspending || owner->namespace_calls || owner->finishing_resume) return false;
    if (!owner->pending_token) {
        if (g_next_token == std::numeric_limits<uint64_t>::max()) return false;
        owner->pending_token = g_next_token++;
    }
    const auto previous = owner->previous_token;
    const auto candidate = owner->pending_token;
    const bool needs_rebound = previous && (!owner->store_namespace.empty() || owner->slot_operation_allowed);
    const auto rebound = g_store_rebound_try;
    if (needs_rebound && g_store_rebound && !rebound) return false;
    owner->finishing_resume = true;
    lock.unlock();
    bool completed = true;
    try { if (needs_rebound && rebound) completed = rebound(previous, candidate); }
    catch (...) {
        lock.lock();
        owner->finishing_resume = false;
        throw;
    }
    lock.lock();
    owner->finishing_resume = false;
    if (!completed) return false;
    owner->token = candidate;
    owner->pending_token = 0;
    return true;
}
bool owner_suspend_begin(void* identity) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = identity == nullptr ? nullptr : find_identity(identity);
    if (owner == nullptr) return false;
    if (owner->token == 0) return true;
    owner->suspending = true;
    owner->stopped_notified = false;
    owner->route_notified = false;
    owner->previous_token = owner->token;
    owner->token = 0;
    ++owner->generation;
    g_condition.notify_all();
    return true;
}
static bool suspend_poll(void* identity, bool blocking_handlers) {
    std::unique_lock<std::mutex> lock(g_mutex);
    auto* owner = identity == nullptr ? nullptr : find_identity(identity);
    if (!owner || owner->token != 0) return false;
    if (!owner->suspending) return true;
    if (owner->namespace_calls != 0 || owner->finishing_suspend) return false;
    owner->finishing_suspend = true;
    const uint64_t stopped_token = owner->previous_token;
    const auto store_stopped = g_store_stopped;
    const auto route_stopped = g_route_stopped;
    const auto store_try = g_store_stopped_try;
    const auto route_try = g_route_stopped_try;
    bool stopped_done = owner->stopped_notified;
    bool route_done = owner->route_notified;
    lock.unlock();
    try {
        if (!stopped_done) {
            if (blocking_handlers && store_stopped) { store_stopped(stopped_token); stopped_done = true; }
            else if (store_try) stopped_done = store_try(stopped_token);
            else stopped_done = store_stopped == nullptr;
        }
        if (!route_done) {
            if (blocking_handlers && route_stopped) { route_stopped(stopped_token); route_done = true; }
            else if (route_try) route_done = route_try(stopped_token);
            else route_done = route_stopped == nullptr;
        }
    } catch (...) {
        lock.lock();
        owner->finishing_suspend = false;
        lock.unlock();
        g_condition.notify_all();
        throw;
    }
    lock.lock();
    owner->stopped_notified = stopped_done;
    owner->route_notified = route_done;
    owner->suspending = !stopped_done || !route_done;
    owner->finishing_suspend = false;
    lock.unlock();
    g_condition.notify_all();
    return stopped_done && route_done;
}
bool owner_suspend_poll(void* identity) { return suspend_poll(identity, false); }
void owner_suspend(void* identity) {
    if (!owner_suspend_begin(identity)) return;
    for (;;) {
        if (suspend_poll(identity, true)) return;
        std::unique_lock<std::mutex> lock(g_mutex);
        auto* owner = find_identity(identity);
        if (!owner) return;
        if (!owner->suspending) return;
        g_condition.wait(lock, [&] {
            return owner->namespace_calls == 0 && !owner->finishing_suspend;
        });
    }
}
bool owner_bind_store(void* identity, const char* store_namespace) {
    if (store_namespace == nullptr || *store_namespace == '\0') return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = identity == nullptr ? nullptr : find_identity(identity);
    if (owner == nullptr) return false;
    if (!owner->store_namespace.empty() && owner->store_namespace != store_namespace) return false;
    owner->store_namespace = store_namespace;
    return true;
}
QolResult owner_verify_token(contracts::QolOwnerToken token) { return verify(nullptr, token); }
QolResult owner_plugin_id_for_token(contracts::QolOwnerToken token, std::string* out_plugin_id) {
    if (out_plugin_id == nullptr) return fail(kQolErrInvalidArgument, "owner plugin id output required");
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto* owner = find_token(token);
    if (owner == nullptr || owner->plugin_id.empty())
        return fail(kQolErrBlocked, "owner token invalid");
    *out_plugin_id = owner->plugin_id;
    return contracts::ok();
}
QolResult owner_namespace_call_begin(contracts::QolOwnerToken token,
                                     OwnerNamespaceCall* out_call) {
    if (out_call == nullptr) return fail(kQolErrInvalidArgument, "owner namespace lease required");
    *out_call = OwnerNamespaceCall{};
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = find_token(token);
    if (owner == nullptr || owner->suspending || owner->plugin_id.empty())
        return fail(kQolErrBlocked, "owner token invalid");
    if (owner->namespace_calls == std::numeric_limits<std::size_t>::max())
        return fail(kQolErrBlocked, "owner namespace calls exhausted");
    ++owner->namespace_calls;
    out_call->identity = owner->identity;
    out_call->plugin_id = owner->plugin_id;
    out_call->active = true;
    return contracts::ok();
}

void owner_namespace_call_end(OwnerNamespaceCall* call) {
    if (call == nullptr || !call->active) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto* owner = find_identity(call->identity);
        if (owner != nullptr && owner->namespace_calls != 0) --owner->namespace_calls;
        call->active = false;
    }
    g_condition.notify_all();
}
bool owner_bind_slot_operation(void* identity) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = identity == nullptr ? nullptr : find_identity(identity);
    if (owner == nullptr) return false;
    owner->slot_operation_allowed = true;
    return true;
}
QolResult owner_authorize_slot_operation(contracts::QolOwnerToken token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto* owner = find_token(token);
    return owner != nullptr && owner->slot_operation_allowed ? contracts::ok()
        : fail(kQolErrBlocked, "slot operation not authorized");
}
bool owner_bind_save_route(void* identity) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = identity == nullptr ? nullptr : find_identity(identity);
    if (owner == nullptr) return false;
    owner->save_route_allowed = true;
    return true;
}
QolResult owner_authorize_save_route(contracts::QolOwnerToken token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto* owner = find_token(token);
    return owner != nullptr && owner->save_route_allowed ? contracts::ok()
        : fail(kQolErrBlocked, "save route not authorized");
}
QolResult owner_authorize_store(contracts::QolOwnerToken token, const char* store_namespace) {
    if (store_namespace == nullptr) return fail(kQolErrInvalidArgument, "invalid store namespace");
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = find_token(token);
    if (owner == nullptr || owner->store_namespace != store_namespace)
        return fail(kQolErrBlocked, "owner store not authorized");
    return contracts::ok();
}
const contracts::QolOwnerIdentityApi& owner_identity_api() { return kApi; }
void owner_identity_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& owner : g_owners) owner = Owner{};
}
void owner_store_lifecycle_handlers(void (*stopped)(uint64_t), void (*rebound)(uint64_t, uint64_t)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_store_stopped = stopped;
    g_store_rebound = rebound;
}
void owner_save_route_stopped_handler(void (*stopped)(uint64_t)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_route_stopped = stopped;
}
void owner_store_lifecycle_try_handlers(bool (*stopped)(uint64_t), bool (*rebound)(uint64_t, uint64_t)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_store_stopped_try = stopped;
    g_store_rebound_try = rebound;
}
void owner_save_route_stopped_try_handler(bool (*stopped)(uint64_t)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_route_stopped_try = stopped;
}
}  // namespace qol::host
