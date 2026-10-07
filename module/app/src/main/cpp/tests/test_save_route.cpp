#include "host/save_route.h"

#include <cstdio>

#include "host/owner_identity.h"

namespace contracts = qol::contracts;
using namespace qol::contracts;
namespace route = qol::host::save_route;

namespace {
int g_pass = 0, g_fail = 0;
#define CHECK(c) \
    do { \
        if (c) ++g_pass; \
        else { \
            ++g_fail; \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        } \
    } while (0)
}  // namespace

namespace route = qol::host::save_route;

namespace {
int native_calls = 0, render_calls = 0, prepare_calls = 0;
QolResult on_native(void*, const QolSaveNativeCall*, QolSaveDomainStatus*) {
    ++native_calls;
    return contracts::ok();
}
QolResult on_render(void*, uint64_t, const QolSaveAdapterState*, QolSaveDomainStatus*) {
    ++render_calls;
    return contracts::ok();
}
QolResult on_prepare(void*) {
    ++prepare_calls;
    return contracts::ok();
}
QolResult on_prepare_busy(void*) {
    ++prepare_calls;
    return contracts::error(kQolErrBusy, "pending transaction", 18);
}
int identity = 0, stranger = 0, other = 0;

// 在途回调（in_flight=1）内部发起 unbind 的驱动器。
const contracts::QolSaveRouteApi* g_api = nullptr;
QolOwnerToken g_self = 0;
uint64_t g_self_binding = 0;
QolResult on_native_unbind_in_flight(void*, const QolSaveNativeCall*, QolSaveDomainStatus*) {
    ++native_calls;
    return g_api->unbind(g_api->host_ctx, g_self, g_self_binding);
}
}  // namespace

int main() {
    CHECK(qol::host::owner_register(&identity));
    CHECK(qol::host::owner_register(&stranger));
    CHECK(qol::host::owner_register(&other));
    CHECK(qol::host::owner_bind_save_route(&identity));
    QolOwnerToken owner = 0, stranger_token = 0, other_token = 0;
    uint64_t generation = 0;
    auto& identities = qol::host::owner_identity_api();
    CHECK(identities.query(&identity, &owner, &generation).ok == kQolOk);
    CHECK(identities.query(&stranger, &stranger_token, &generation).ok == kQolOk);
    CHECK(identities.query(&other, &other_token, &generation).ok == kQolOk);
    const auto& api = route::save_route_api();

    // 未授权 owner bind → Blocked。
    QolSaveOwnerCallbacks callbacks{};
    callbacks.abi.size = sizeof(callbacks);
    callbacks.native_call = &on_native;
    callbacks.render_ready = &on_render;
    callbacks.prepare_stop = &on_prepare;
    uint64_t binding = 0;
    CHECK(api.bind(api.host_ctx, stranger_token, &callbacks, &binding).code == kQolErrBlocked);
    CHECK(api.bind(api.host_ctx, owner, nullptr, &binding).code == kQolErrInvalidArgument);
    QolSaveOwnerCallbacks incomplete = callbacks;
    incomplete.render_ready = nullptr;
    CHECK(api.bind(api.host_ctx, owner, &incomplete, &binding).code == kQolErrInvalidArgument);

    // bind 复制表：绑定后改写外部表不影响已注册回调。
    QolSaveOwnerCallbacks mutated = callbacks;
    CHECK(api.bind(api.host_ctx, owner, &mutated, &binding).ok == kQolOk);
    QolSaveOwnerCallbacks poison = callbacks;
    poison.native_call = nullptr;
    mutated = poison;
    QolSaveNativeCall call{};
    call.size = sizeof(call);
    QolSaveDomainStatus status{};
    status.size = sizeof(status);
    CHECK(route::dispatch_native_call(owner, &call, &status).ok == kQolOk);
    CHECK(native_calls == 1);

    // 同 owner 重复 bind → Busy；未绑定 owner 派发 → NotFound。
    uint64_t second = 0;
    CHECK(api.bind(api.host_ctx, owner, &callbacks, &second).code == kQolErrBusy);
    CHECK(route::dispatch_native_call(other_token, &call, &status).code == kQolErrBlocked);
    CHECK(route::dispatch_native_call(stranger_token, &call, &status).code == kQolErrBlocked);
    QolSaveAdapterState adapter_state{};
    adapter_state.size = sizeof(adapter_state);
    CHECK(route::dispatch_render_ready(owner, 7, &adapter_state, &status).ok == kQolOk);
    CHECK(render_calls == 1);

    // prepare_stop 透传：owner 回调 Busy 时宿主生命周期操作明确失败。
    CHECK(route::prepare_stop(other_token).code == kQolErrBlocked);
    CHECK(route::prepare_stop(owner).ok == kQolOk);
    QolSaveOwnerCallbacks busy_callbacks = callbacks;
    busy_callbacks.prepare_stop = &on_prepare_busy;
    uint64_t busy_binding = 0;
    CHECK(api.unbind(api.host_ctx, owner, binding).ok == kQolOk);
    CHECK(api.bind(api.host_ctx, owner, &busy_callbacks, &busy_binding).ok == kQolOk);
    CHECK(route::prepare_stop(owner).code == kQolErrBusy);

    // prepare_stop 缺失 → Unsupported。
    QolSaveOwnerCallbacks no_prepare = callbacks;
    no_prepare.prepare_stop = nullptr;
    uint64_t no_prepare_binding = 0;
    CHECK(api.unbind(api.host_ctx, owner, busy_binding).ok == kQolOk);
    CHECK(api.bind(api.host_ctx, owner, &no_prepare, &no_prepare_binding).ok == kQolOk);
    CHECK(route::prepare_stop(owner).code == kQolErrUnsupported);
    CHECK(api.unbind(api.host_ctx, owner, no_prepare_binding).ok == kQolOk);
    CHECK(api.bind(api.host_ctx, owner, &callbacks, &binding).ok == kQolOk);

    // unbind 后零回调：binding 摘除后 dispatch NotFound，fake 计数不再增长。
    CHECK(api.unbind(api.host_ctx, stranger_token, binding).code == kQolErrBlocked);
    CHECK(api.unbind(api.host_ctx, owner, 0).code == kQolErrNotFound);
    CHECK(api.unbind(api.host_ctx, owner, binding).ok == kQolOk);
    const int native_before = native_calls;
    CHECK(route::dispatch_native_call(owner, &call, &status).code == kQolErrNotFound);
    CHECK(native_calls == native_before);
    // 重复 unbind（已收尾）→ NotFound。
    CHECK(api.unbind(api.host_ctx, owner, binding).code == kQolErrNotFound);

    // 在途回调期间 unbind → NotReady；draining 后零派发；帧点 step 完成清理。
    g_api = &api;
    QolSaveOwnerCallbacks draining_callbacks = callbacks;
    draining_callbacks.native_call = &on_native_unbind_in_flight;
    uint64_t draining_binding = 0;
    CHECK(api.bind(api.host_ctx, owner, &draining_callbacks, &draining_binding).ok == kQolOk);
    g_self = owner;
    g_self_binding = draining_binding;
    // 回调执行中（in_flight=1）内部 unbind：接受为 draining，NotReady 经派发透传。
    CHECK(route::dispatch_native_call(owner, &call, &status).code == kQolErrNotReady);
    CHECK(native_calls == native_before + 1);
    // draining 状态：不再派发、重复 unbind NotReady、step 后条目清理。
    CHECK(route::dispatch_native_call(owner, &call, &status).code == kQolErrNotFound);
    CHECK(native_calls == native_before + 1);
    CHECK(api.unbind(api.host_ctx, owner, draining_binding).code == kQolErrNotReady);
    route::step();
    CHECK(api.unbind(api.host_ctx, owner, draining_binding).code == kQolErrNotFound);

    // owner 停用：binding 退出派发；resume 后新 token 重新 bind 恢复。
    uint64_t suspended_binding = 0;
    CHECK(api.bind(api.host_ctx, owner, &callbacks, &suspended_binding).ok == kQolOk);
    qol::host::owner_suspend(&identity);
    CHECK(route::dispatch_native_call(owner, &call, &status).code == kQolErrBlocked);
    route::step();
    CHECK(qol::host::owner_resume(&identity));
    CHECK(identities.query(&identity, &owner, &generation).ok == kQolOk);
    uint64_t rebound = 0;
    CHECK(api.bind(api.host_ctx, owner, &callbacks, &rebound).ok == kQolOk);
    CHECK(route::dispatch_native_call(owner, &call, &status).ok == kQolOk);
    CHECK(api.unbind(api.host_ctx, owner, rebound).ok == kQolOk);
    route::reset_for_test();

    std::printf("save_route_tests: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
