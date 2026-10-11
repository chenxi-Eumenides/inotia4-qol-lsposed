#include "host/http_dispatch.h"
#include "host/http_registry.h"

#include <atomic>
#include <chrono>
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

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

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

void test_owner_unregister_waits() {
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

    std::atomic<bool> unregister_done{false};
    std::atomic<bool> unregister_ok{false};
    std::thread unregister_thread([&] {
        unregister_ok.store(dispatch.unregister_owner("owner").ok == 0);
        unregister_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!unregister_done.load());
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
    unregister_thread.join();
    CHECK(request_ok.load());
    CHECK(unregister_ok.load());
    CHECK(unregister_done.load());
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

void test_api_service_gates_operation_routes() {
    reset();
    setup_post_route("/api/op/features/lifecycle", "owner", 1);
    const auto dispatch = qol::host::http_dispatch_api();
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/op/features/lifecycle", "owner",
                                    &command_handler, nullptr).ok == 0);

    uint8_t storage[128]{};
    HttpRes output = response(storage, sizeof(storage));
    HttpReq input = post_request("/api/op/features/lifecycle", "{\"id\":\"simple_mode\"}");
    // 默认关闭：op_gated routes fail closed before handler dispatch.
    CHECK(dispatch.dispatch(&input, &output).code == kQolErrBlocked);
    CHECK(output.status == 403);
    CHECK(std::string(reinterpret_cast<char*>(storage), output.body_size) ==
          "{\"ok\":false,\"error\":\"op disabled\"}");

    qol::host::http_dispatch_set_api_enabled(true);
    output = response(storage, sizeof(storage));
    CHECK(dispatch.dispatch(&input, &output).ok == 0);
    CHECK(output.status == 200);

    qol::host::http_dispatch_set_api_enabled(false);
    output = response(storage, sizeof(storage));
    CHECK(dispatch.dispatch(&input, &output).code == kQolErrBlocked);
    CHECK(output.status == 403);

    // Non-operation routes remain available to direct dispatch independently of this gate.
    setup_post_route("/api/quest/quit_quest", "core.quest", 0);
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/quest/quit_quest", "core.quest",
                                    &command_handler, nullptr).ok == 0);
    output = response(storage, sizeof(storage));
    input = post_request("/api/quest/quit_quest", "{}");
    CHECK(dispatch.dispatch(&input, &output).ok == 0);
    CHECK(output.status == 200);
}

}  // namespace

int main() {
    test_template_and_query();
    test_owner_unregister_waits();
    test_inactive_and_missing_handler();
    test_command_registration_and_dispatch();
    test_command_tombstone_and_restore();
    test_api_service_gates_operation_routes();
    std::printf("http_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
