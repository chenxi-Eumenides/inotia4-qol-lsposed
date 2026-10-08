#include "host/save_route.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#include "host/owner_identity.h"

namespace qol::host::save_route {
namespace {

constexpr uint32_t kMaxBindings = 8;

struct Binding {
    uint64_t id;
    contracts::QolOwnerToken owner;
    contracts::QolSaveOwnerCallbacks callbacks;  // bind 时复制的快照
    bool draining;                               // unbind 已接受，等在途回调归零
    uint32_t in_flight;                          // 正在锁外执行的回调数
};

std::mutex g_mtx;
std::array<Binding, kMaxBindings> g_bindings{};
uint64_t g_next_id = 1;
// 装配期注入的「插件 id → owner token」查询缝（dispatch_native_call_for_plugin 用）。
std::atomic<PluginTokenQuery> g_plugin_token_query{nullptr};

QolResult fail(int32_t code, const char* text) {
    return contracts::error(code, text, static_cast<uint32_t>(std::strlen(text)));
}

Binding* find_by_id_locked(uint64_t id) {
    if (id == 0) return nullptr;
    for (auto& binding : g_bindings)
        if (binding.id == id) return &binding;
    return nullptr;
}

Binding* find_active_by_owner_locked(contracts::QolOwnerToken owner) {
    for (auto& binding : g_bindings)
        if (binding.id != 0 && !binding.draining && binding.owner == owner) return &binding;
    return nullptr;
}

// 派发与 prepare_stop 共用的转体：锁内快照回调表，锁外调用，回锁清算 in_flight。
template <typename Fn>
QolResult invoke_active_locked(std::mutex& mtx, contracts::QolOwnerToken owner, Fn&& invoke) {
    if (!contracts::succeeded(owner_authorize_save_route(owner)))
        return fail(kQolErrBlocked, "save route not authorized");
    Binding* binding = find_active_by_owner_locked(owner);
    if (binding == nullptr) return fail(kQolErrNotFound, "no active save route binding");
    // 快照后锁外调用；unbind 只对 draining 之前的活跃 binding 派发，保证 unbind 后零回调。
    const contracts::QolSaveOwnerCallbacks callbacks = binding->callbacks;
    void* user = callbacks.user;
    ++binding->in_flight;
    mtx.unlock();
    const QolResult result = invoke(callbacks, user);
    mtx.lock();
    --binding->in_flight;
    return result;
}

QolResult route_bind(void*, contracts::QolOwnerToken owner,
                     const contracts::QolSaveOwnerCallbacks* callbacks, uint64_t* out_binding) {
    if (out_binding == nullptr || callbacks == nullptr ||
        callbacks->abi.size < sizeof(*callbacks) || callbacks->native_call == nullptr ||
        callbacks->render_ready == nullptr)
        return fail(kQolErrInvalidArgument, "invalid save route callbacks");
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!contracts::succeeded(owner_authorize_save_route(owner)))
        return fail(kQolErrBlocked, "save route not authorized");
    if (find_active_by_owner_locked(owner) != nullptr)
        return fail(kQolErrBusy, "owner already bound");
    if (g_next_id == UINT64_MAX) return fail(kQolErrBusy, "binding id exhausted");
    for (auto& binding : g_bindings) {
        if (binding.id != 0) continue;
        // bind 只复制表，不在调用栈访问游戏或回调（冻结语义）。
        binding = Binding{g_next_id, owner, *callbacks, false, 0};
        *out_binding = g_next_id++;
        return contracts::ok();
    }
    return fail(kQolErrBusy, "binding capacity exhausted");
}

QolResult route_unbind(void*, contracts::QolOwnerToken owner, uint64_t binding_id) {
    std::lock_guard<std::mutex> lock(g_mtx);
    Binding* binding = find_by_id_locked(binding_id);
    if (binding == nullptr) return fail(kQolErrNotFound, "unknown save route binding");
    if (binding->owner != owner) return fail(kQolErrBlocked, "binding owned by another owner");
    if (binding->draining) return fail(kQolErrNotReady, "unbind still draining");
    if (binding->in_flight != 0) {
        // 在途回调执行中：置 draining 后不再派发，帧点 step 在归零后清理。
        binding->draining = true;
        return fail(kQolErrNotReady, "unbind still draining");
    }
    *find_by_id_locked(binding_id) = Binding{};
    return contracts::ok();
}

QolResult route_dispatch_native(void*, contracts::QolOwnerToken owner,
                                const contracts::QolSaveNativeCall* call,
                                contracts::QolSaveDomainStatus* out_status) {
    if (call == nullptr || call->size < sizeof(*call))
        return fail(kQolErrInvalidArgument, "invalid native call");
    std::lock_guard<std::mutex> lock(g_mtx);
    return invoke_active_locked(
        g_mtx, owner, [&](const contracts::QolSaveOwnerCallbacks& cb, void* user) {
            return cb.native_call(user, call, out_status);
        });
}

QolResult route_dispatch_render_ready(void*, contracts::QolOwnerToken owner, uint64_t transition,
                                      const contracts::QolSaveAdapterState* adapter_state,
                                      contracts::QolSaveDomainStatus* out_status) {
    if (adapter_state == nullptr || adapter_state->size < sizeof(*adapter_state))
        return fail(kQolErrInvalidArgument, "invalid adapter state");
    std::lock_guard<std::mutex> lock(g_mtx);
    return invoke_active_locked(
        g_mtx, owner, [&](const contracts::QolSaveOwnerCallbacks& cb, void* user) {
            return cb.render_ready(user, transition, adapter_state, out_status);
        });
}

QolResult route_prepare_stop(void*, contracts::QolOwnerToken owner) {
    std::lock_guard<std::mutex> lock(g_mtx);
    return invoke_active_locked(
        g_mtx, owner, [&](const contracts::QolSaveOwnerCallbacks& cb, void* user) {
            return cb.prepare_stop != nullptr ? cb.prepare_stop(user)
                                              : fail(kQolErrUnsupported, "prepare_stop missing");
        });
}

struct RouteStopped {
    void operator()(uint64_t token) const {
        // owner 停用：旧 token 的 binding 立即退出派发；等在途回调归零后由 step 清理。
        std::lock_guard<std::mutex> lock(g_mtx);
        for (auto& binding : g_bindings)
            if (binding.id != 0 && binding.owner == token) binding.draining = true;
    }
};

const contracts::QolSaveRouteApi kApi{
    {sizeof(contracts::QolSaveRouteApi), 1, 0}, nullptr, &route_bind, &route_unbind,
    &route_dispatch_native};

}  // namespace

const contracts::QolSaveRouteApi& save_route_api() { return kApi; }

QolResult dispatch_native_call(contracts::QolOwnerToken owner,
                               const contracts::QolSaveNativeCall* call,
                               contracts::QolSaveDomainStatus* out_status) {
    return route_dispatch_native(nullptr, owner, call, out_status);
}

QolResult dispatch_native_call_for_plugin(const char* plugin_id,
                                          const contracts::QolSaveNativeCall* call,
                                          contracts::QolSaveDomainStatus* out_status) {
    // token 查询经装配期注入缝：save_route 不反向依赖 plugin_registry（测试目标
    // 不链接 registry）。
    const PluginTokenQuery query = g_plugin_token_query.load(std::memory_order_acquire);
    const contracts::QolOwnerToken token =
        query != nullptr && plugin_id != nullptr ? query(plugin_id) : 0;
    if (token == 0) return fail(kQolErrNotFound, "plugin owner unknown");
    return route_dispatch_native(nullptr, token, call, out_status);
}

void set_plugin_token_query(PluginTokenQuery query) {
    g_plugin_token_query.store(query, std::memory_order_release);
}

QolResult dispatch_render_ready(contracts::QolOwnerToken owner, uint64_t transition,
                                const contracts::QolSaveAdapterState* adapter_state,
                                contracts::QolSaveDomainStatus* out_status) {
    return route_dispatch_render_ready(nullptr, owner, transition, adapter_state, out_status);
}

QolResult prepare_stop(contracts::QolOwnerToken owner) {
    return route_prepare_stop(nullptr, owner);
}

void step() {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& binding : g_bindings) {
        if (binding.id == 0 || !binding.draining || binding.in_flight != 0) continue;
        binding = Binding{};
    }
}

void owner_stopped(uint64_t token) { RouteStopped{}(token); }

void reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& binding : g_bindings) binding = Binding{};
    g_next_id = 1;
}

}  // namespace qol::host::save_route
