#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "infra/command_queue.h"
#include "infra/commands.h"
#include "infra/request_policy.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using qol::contracts::HttpMethod;
using qol::contracts::HttpReq;
using qol::contracts::HttpRes;
using qol::contracts::HttpRoute;
using qol::contracts::QolCommandTicket;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

bool g_security_op_enabled = false;
uint32_t g_capacity_handler_calls = 0;

constexpr uint32_t kQueuedHttpHandlerId = 71;

struct QueuedHttpContext {
    uint32_t handler_calls = 0;
    QolCommandTicket ticket = 0;
};

struct QueuedBusinessContext {
    uint32_t calls = 0;
};

bool test_security_authorizer() { return g_security_op_enabled; }

void write_body(HttpRes* response, const std::string& body) {
    response->status = 200;
    if (response->body_capacity >= body.size()) {
        std::memcpy(response->body, body.data(), body.size());
        response->body_size = static_cast<uint32_t>(body.size());
    }
}

QolResult query_handler(const HttpReq* request, HttpRes* response, void*) {
    if (request->path_param_count != 1 || request->path_params == nullptr) {
        return qol::contracts::error(kQolErrInternal, "missing path parameter", 22);
    }
    const auto& param = request->path_params[0];
    const std::string slot(param.value, param.value_len);
    const std::string query = request->query == nullptr
                                  ? std::string()
                                  : std::string(request->query, request->query_len);
    write_body(response, "{\"slot\":\"" + slot + "\",\"query\":\"" + query + "\"}");
    return qol::contracts::ok();
}

struct BlockingContext {
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool release = false;
};

QolResult command_handler(const HttpReq* request, HttpRes* response, void*) {
    // 只回显 body 长度，便于断言 POST body 经信封完整送达。
    write_body(response, "{\"cmd\":true,\"len\":" + std::to_string(request->body_len) + "}");
    return qol::contracts::ok();
}

QolResult capacity_command_handler(const HttpReq*, HttpRes* response, void*) {
    ++g_capacity_handler_calls;
    write_body(response, "{\"capacity\":true}");
    return qol::contracts::ok();
}

QolResult queued_command_handler(const HttpReq*, HttpRes* response, void* raw_ctx) {
    auto* context = static_cast<QueuedHttpContext*>(raw_ctx);
    ++context->handler_calls;
    const uint8_t payload = 1;
    const QolResult posted = qol::infra::commands_port().post_typed(
        kQueuedHttpHandlerId, &payload, sizeof(payload), &context->ticket);
    if (!qol::contracts::succeeded(posted)) return posted;
    write_body(response, "{\"ticket\":" + std::to_string(context->ticket) + "}");
    return qol::contracts::ok();
}

bool queued_business_handler(void* raw_ctx, const uint8_t*, uint32_t, char* out,
                             uint32_t capacity, uint32_t* out_size) {
    auto* context = static_cast<QueuedBusinessContext*>(raw_ctx);
    ++context->calls;
    static constexpr char kResult[] = "{\"ok\":true,\"executed\":true}";
    if (out == nullptr || out_size == nullptr || capacity < sizeof(kResult) - 1) return false;
    std::memcpy(out, kResult, sizeof(kResult) - 1);
    *out_size = sizeof(kResult) - 1;
    return true;
}

bool ticket_result_is(QolCommandTicket ticket, const char* expected) {
    char result[128]{};
    uint32_t result_size = 0;
    if (!qol::contracts::succeeded(qol::infra::commands_read_result(
            ticket, result, sizeof(result) - 1, &result_size))) return false;
    result[result_size] = '\0';
    return std::strcmp(result, expected) == 0;
}

QolResult blocking_handler(const HttpReq*, HttpRes* response, void* raw_ctx) {
    auto* context = static_cast<BlockingContext*>(raw_ctx);
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->started = true;
        context->cv.notify_all();
    }
    std::unique_lock<std::mutex> lock(context->mutex);
    context->cv.wait(lock, [context] { return context->release; });
    write_body(response, "{\"ok\":true}");
    return qol::contracts::ok();
}

bool resident_config_route(const HttpReq& request) {
    constexpr char kPath[] = "/api/config/list";
    return request.method == HttpMethod::kGet && request.path != nullptr &&
           request.path_len == sizeof(kPath) - 1 &&
           std::memcmp(request.path, kPath, sizeof(kPath) - 1) == 0;
}

QolResult resident_config_dispatch(const HttpReq&, HttpRes& response) {
    write_body(&response, "{\"resident\":true}");
    return qol::contracts::ok();
}

HttpReq request(const char* path, const char* query = nullptr) {
    return HttpReq{HttpMethod::kGet,
                   path,
                   static_cast<uint32_t>(std::strlen(path)),
                   nullptr,
                   0,
                   query,
                   query == nullptr ? 0U : static_cast<uint32_t>(std::strlen(query)),
                   nullptr,
                   0};
}

HttpReq post_request(const char* path, const char* body) {
    return HttpReq{HttpMethod::kPost,
                   path,
                   static_cast<uint32_t>(std::strlen(path)),
                   reinterpret_cast<const uint8_t*>(body),
                   body == nullptr ? 0U : static_cast<uint32_t>(std::strlen(body)),
                   nullptr,
                   0,
                   nullptr,
                   0};
}

HttpRes response(uint8_t* body, uint32_t capacity) {
    return HttpRes{body, capacity, 0, 0};
}

void setup_route(const char* path, const char* owner = "owner") {
    const auto registry = qol::host::http_registry_api();
    const HttpRoute route{HttpMethod::kGet, path, owner, 0, 1};
    CHECK(registry.register_route(&route).ok == 0);
}

void setup_post_route(const char* path, const char* owner = "owner", uint8_t op_gated = 0) {
    const auto registry = qol::host::http_registry_api();
    const HttpRoute route{HttpMethod::kPost, path, owner, op_gated, 1};
    CHECK(registry.register_route(&route).ok == 0);
}

void reset() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_capacity_handler_calls = 0;
}

void test_template_and_query() {
    reset();
    setup_route("/api/party/{slot}");
    const auto dispatch = qol::host::http_dispatch_api();
    CHECK(dispatch.register_query(HttpMethod::kGet, "/api/party/{slot}", "owner",
                                  &query_handler, nullptr).ok == 0);
    CHECK(dispatch.register_query(HttpMethod::kGet, "/api/party/{slot}", "owner",
                                  &query_handler, nullptr).code == kQolErrConflict);
    // command 注册要求非空 handler 与 kPost 路由：空 handler 是参数错误（不再是 unsupported）。
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/party/{slot}", "owner", nullptr,
                                    nullptr).code == kQolErrInvalidArgument);

    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = request("/api/party/3", "verbose=1");
    CHECK(dispatch.dispatch(&input, &output).ok == 0);
    CHECK(output.status == 200);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"slot\":\"3\",\"query\":\"verbose=1\"}");

    output = response(storage, sizeof(storage));
    input = request("/api/party/3/extra");
    CHECK(dispatch.dispatch(&input, &output).code == kQolErrNotFound);
    CHECK(output.status == 404);
}

void test_owner_unregister_is_nonblocking() {
    reset();
    setup_route("/api/block");
    const auto dispatch = qol::host::http_dispatch_api();
    BlockingContext context;
    CHECK(dispatch.register_query(HttpMethod::kGet, "/api/block", "owner",
                                  &blocking_handler, &context).ok == 0);

    uint8_t storage[64]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = request("/api/block");
    std::atomic<bool> request_ok{false};
    std::thread request_thread([&] { request_ok.store(dispatch.dispatch(&input, &output).ok == 0); });
    {
        std::unique_lock<std::mutex> lock(context.mutex);
        context.cv.wait(lock, [&context] { return context.started; });
    }

    CHECK(dispatch.unregister_owner("owner").code == kQolErrBusy);
    uint8_t stopped_storage[64]{};
    HttpRes stopped_output = response(stopped_storage, sizeof(stopped_storage));
    HttpReq stopped_input = request("/api/block");
    CHECK(dispatch.dispatch(&stopped_input, &stopped_output).code == kQolErrNotReady);
    CHECK(stopped_output.status == 503);
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.release = true;
        context.cv.notify_all();
    }
    request_thread.join();
    CHECK(request_ok.load());
    CHECK(dispatch.unregister_owner("owner").ok == 0);
}

void test_inactive_and_missing_handler() {
    reset();
    setup_route("/api/no-handler");
    const auto dispatch = qol::host::http_dispatch_api();
    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = request("/api/no-handler");
    CHECK(dispatch.dispatch(&input, &output).code == kQolErrNotFound);
    CHECK(output.status == 404);

    CHECK(qol::host::http_registry_api().unregister_owner(nullptr).ok == 0);
    output = response(storage, sizeof(storage));
    CHECK(dispatch.dispatch(&input, &output).code == kQolErrNotReady);
    CHECK(output.status == 503);
}

void test_resident_route_bypasses_plugin_handler_registry() {
    reset();
    const auto registry = qol::host::http_registry_api();
    const HttpRoute config_route{HttpMethod::kGet, "/api/config/list", "settings", 0, 1};
    CHECK(registry.register_route(&config_route).ok == 0);
    const HttpRoute* registered = nullptr;
    CHECK(registry.find(HttpMethod::kGet, "/api/config/list", &registered).ok == 0);
    CHECK(registered != nullptr && std::strcmp(registered->owner, "settings") == 0);

    // Resident config dispatch must work without a settings plugin handler slot.
    qol::host::http_dispatch_bind_resident_control(&resident_config_route,
                                                   &resident_config_dispatch);
    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = request("/api/config/list");
    CHECK(qol::host::http_dispatch_api().dispatch(&input, &output).ok == 0);
    CHECK(output.status == 200);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"resident\":true}");
    qol::host::http_dispatch_bind_resident_control(nullptr, nullptr);
    reset();
}

void test_command_registration_and_dispatch() {
    reset();
    setup_post_route("/api/op/features/lifecycle");
    const auto dispatch = qol::host::http_dispatch_api();
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/op/features/lifecycle", "owner",
                                    &command_handler, nullptr).ok == 0);
    // 重复注册同 method+path 冲突；GET 注册命令不合法。
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/op/features/lifecycle", "owner",
                                    &command_handler, nullptr).code == kQolErrConflict);
    CHECK(dispatch.register_command(HttpMethod::kGet, "/api/op/features/lifecycle", "owner",
                                    &command_handler, nullptr).code == kQolErrInvalidArgument);

    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = post_request("/api/op/features/lifecycle", "{\"id\":\"simple_mode\"}");
    CHECK(dispatch.dispatch(&input, &output).ok == 0);
    CHECK(output.status == 200);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"cmd\":true,\"len\":20}");

    // command 槽不响应 GET。
    output = response(storage, sizeof(storage));
    input = request("/api/op/features/lifecycle");
    CHECK(dispatch.dispatch(&input, &output).code == kQolErrNotFound);
    CHECK(output.status == 404);
}

void test_command_tombstone_and_restore() {
    reset();
    setup_post_route("/api/op/features/lifecycle");
    const auto dispatch = qol::host::http_dispatch_api();
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/op/features/lifecycle", "owner",
                                    &command_handler, nullptr).ok == 0);

    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = post_request("/api/op/features/lifecycle", "{}");
    CHECK(dispatch.unregister_owner("owner").ok == 0);
    CHECK(dispatch.dispatch(&input, &output).code == kQolErrNotReady);
    CHECK(output.status == 503);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"ok\":false,\"error\":\"http owner inactive\"}");

    // 墓碑可被同 owner 重新注册（恢复）。
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/op/features/lifecycle", "owner",
                                    &command_handler, nullptr).ok == 0);
    output = response(storage, sizeof(storage));
    input = post_request("/api/op/features/lifecycle", "{}");
    CHECK(dispatch.dispatch(&input, &output).ok == 0);
    CHECK(output.status == 200);
}

void test_op_routes_use_independent_host_security() {
    reset();
    setup_post_route("/api/op/features/lifecycle", "owner", 1);
    setup_post_route("/api/quest/quit_quest", "core.quest", 0);
    const auto dispatch = qol::host::http_dispatch_api();
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/op/features/lifecycle", "owner",
                                    &command_handler, nullptr).ok == 0);
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/quest/quit_quest", "core.quest",
                                    &command_handler, nullptr).ok == 0);

    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = post_request("/api/op/features/lifecycle", "{\"id\":\"simple_mode\"}");
    // A not-ready security query fails closed, regardless of API listener compatibility state.
    qol::host::http_dispatch_set_api_enabled(true);
    CHECK(dispatch.dispatch(&input, &output).code == kQolErrBlocked);
    CHECK(output.status == 403);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"ok\":false,\"error\":\"op disabled\"}");

    qol::host::http_dispatch_set_op_authorizer_for_test(&test_security_authorizer);
    for (const bool api_enabled : std::array<bool, 2>{false, true}) {
        qol::host::http_dispatch_set_api_enabled(api_enabled);
        for (const bool op_enabled : std::array<bool, 2>{false, true}) {
            g_security_op_enabled = op_enabled;

            output = response(storage, sizeof(storage));
            input = post_request("/api/op/features/lifecycle", "{\"id\":\"simple_mode\"}");
            const QolResult op_result = dispatch.dispatch(&input, &output);
            CHECK(op_enabled ? op_result.ok == 0 : op_result.code == kQolErrBlocked);
            CHECK(output.status == (op_enabled ? 200 : 403));
            if (!op_enabled) {
                CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
                      "{\"ok\":false,\"error\":\"op disabled\"}");
            }

            // Non-OP routes remain available to direct dispatch for every API/OP combination.
            output = response(storage, sizeof(storage));
            input = post_request("/api/quest/quit_quest", "{}");
            CHECK(dispatch.dispatch(&input, &output).ok == 0);
            CHECK(output.status == 200);
        }
    }
}

void test_lease_capacity_returns_service_unavailable_without_handler_call() {
    constexpr uint32_t kLeaseCapacity = 256;
    reset();
    setup_post_route("/api/op/capacity", "owner", 1);
    const auto dispatch = qol::host::http_dispatch_api();
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/op/capacity", "owner",
                                    &capacity_command_handler, nullptr).ok == 0);
    qol::host::http_dispatch_set_op_authorizer_for_test(&test_security_authorizer);

    const auto registry = qol::host::http_registry_api();
    std::array<uint64_t, kLeaseCapacity> leases{};
    for (uint64_t& lease : leases) {
        CHECK(registry.lease_enter(HttpMethod::kPost, "/api/op/capacity", &lease).ok == 0);
        CHECK(lease != 0);
    }

    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = post_request("/api/op/capacity", "{}");
    g_security_op_enabled = true;
    const QolResult capacity_result = dispatch.dispatch(&input, &output);
    CHECK(capacity_result.code == kQolErrBusy);
    CHECK(output.status == 503);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"ok\":false,\"error\":\"http route lease capacity\"}");
    CHECK(g_capacity_handler_calls == 0);

    // The independent OP check remains a 403 even while the lease table is full.
    output = response(storage, sizeof(storage));
    g_security_op_enabled = false;
    const QolResult denied_result = dispatch.dispatch(&input, &output);
    CHECK(denied_result.code == kQolErrBlocked);
    CHECK(output.status == 403);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"ok\":false,\"error\":\"op disabled\"}");
    CHECK(g_capacity_handler_calls == 0);

    CHECK(registry.lease_leave(leases[0]).ok == 0);
    output = response(storage, sizeof(storage));
    g_security_op_enabled = true;
    CHECK(dispatch.dispatch(&input, &output).ok == 0);
    CHECK(output.status == 200 && g_capacity_handler_calls == 1);

    for (uint64_t lease : leases) {
        if (lease != leases[0]) CHECK(registry.lease_leave(lease).ok == 0);
    }
    uint64_t recovered_lease = 0;
    CHECK(registry.lease_enter(HttpMethod::kPost, "/api/op/capacity", &recovered_lease).ok == 0);
    CHECK(recovered_lease != 0);
    CHECK(registry.lease_leave(recovered_lease).ok == 0);

    CHECK(registry.unregister_owner("owner").ok == 0);
    output = response(storage, sizeof(storage));
    const QolResult missing_route_result = dispatch.dispatch(&input, &output);
    CHECK(missing_route_result.code == kQolErrNotFound);
    CHECK(output.status == 404);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"ok\":false,\"error\":\"http route not found\"}");
    CHECK(g_capacity_handler_calls == 1);
}

void test_http_op_policy_is_enforced_when_queued_command_runs() {
    reset();
    qol::infra::commands_reset_for_test();
    qol::infra::command_queue_reset_for_test();
    qol::infra::set_request_op_authorizer(&test_security_authorizer);
    qol::host::http_dispatch_set_op_authorizer_for_test(&test_security_authorizer);

    setup_post_route("/api/op/queued", "op.owner", 1);
    setup_post_route("/api/plugin/queued", "plugin.owner", 0);
    const auto dispatch = qol::host::http_dispatch_api();

    QueuedHttpContext op_context{};
    QueuedHttpContext plugin_context{};
    QueuedBusinessContext business_context{};
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/op/queued", "op.owner",
                                    &queued_command_handler, &op_context).ok == 0);
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/plugin/queued", "plugin.owner",
                                    &queued_command_handler, &plugin_context).ok == 0);
    CHECK(qol::infra::commands_register_typed_result_handler(
              kQueuedHttpHandlerId, &queued_business_handler, &business_context).ok == 0);

    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq op_input = post_request("/api/op/queued", "{}");
    qol::host::http_dispatch_set_api_enabled(false);
    g_security_op_enabled = true;
    CHECK(dispatch.dispatch(&op_input, &output).ok == 0);
    CHECK(output.status == 200 && op_context.handler_calls == 1 && op_context.ticket != 0);

    // Admission was authorized, but execution observes the permission revocation.
    g_security_op_enabled = false;
    CHECK(qol::infra::commands_drain() == 1);
    CHECK(qol::infra::commands_port().state(op_context.ticket) ==
          qol::contracts::kCommandStateFailed);
    CHECK(business_context.calls == 0);
    CHECK(ticket_result_is(op_context.ticket, "{\"ok\":false,\"error\":\"op disabled\"}"));

    // API listener compatibility state does not replace the request's OP policy.
    output = response(storage, sizeof(storage));
    qol::host::http_dispatch_set_api_enabled(true);
    g_security_op_enabled = true;
    CHECK(dispatch.dispatch(&op_input, &output).ok == 0);
    CHECK(output.status == 200 && op_context.ticket != 0);
    CHECK(qol::infra::commands_drain() == 1);
    CHECK(qol::infra::commands_port().state(op_context.ticket) ==
          qol::contracts::kCommandStateDone);
    CHECK(business_context.calls == 1);
    CHECK(ticket_result_is(op_context.ticket, "{\"ok\":true,\"executed\":true}"));

    // OP-off is still rejected at HTTP admission and creates no ticket.
    output = response(storage, sizeof(storage));
    const uint32_t op_handler_calls = op_context.handler_calls;
    op_context.ticket = 0;
    g_security_op_enabled = false;
    const QolResult denied = dispatch.dispatch(&op_input, &output);
    CHECK(denied.code == kQolErrBlocked && output.status == 403);
    CHECK(op_context.handler_calls == op_handler_calls && op_context.ticket == 0);

    // Non-OP plugin commands execute with OP disabled regardless of API compatibility state.
    HttpReq plugin_input = post_request("/api/plugin/queued", "{}");
    for (const bool api_enabled : std::array<bool, 2>{false, true}) {
        qol::host::http_dispatch_set_api_enabled(api_enabled);
        output = response(storage, sizeof(storage));
        g_security_op_enabled = false;
        CHECK(dispatch.dispatch(&plugin_input, &output).ok == 0);
        CHECK(output.status == 200 && plugin_context.ticket != 0);
        CHECK(qol::infra::commands_drain() == 1);
        CHECK(qol::infra::commands_port().state(plugin_context.ticket) ==
              qol::contracts::kCommandStateDone);
        CHECK(business_context.calls == (api_enabled ? 3U : 2U));
    }
    CHECK(qol::infra::commands_unregister_typed_result_handler(kQueuedHttpHandlerId).ok == 0);
}

}  // namespace

int main() {
    test_template_and_query();
    test_owner_unregister_is_nonblocking();
    test_inactive_and_missing_handler();
    test_resident_route_bypasses_plugin_handler_registry();
    test_command_registration_and_dispatch();
    test_command_tombstone_and_restore();
    test_op_routes_use_independent_host_security();
    test_lease_capacity_returns_service_unavailable_without_handler_call();
    test_http_op_policy_is_enforced_when_queued_command_runs();
    std::printf("http_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
