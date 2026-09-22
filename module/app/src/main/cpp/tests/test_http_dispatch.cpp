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

HttpRes response(uint8_t* body, uint32_t capacity) {
    return HttpRes{body, capacity, 0, 0};
}

void setup_route(const char* path, const char* owner = "owner") {
    const auto registry = qol::host::http_registry_api();
    const HttpRoute route{HttpMethod::kGet, path, owner, 0, 1};
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
    CHECK(dispatch.register_command(HttpMethod::kPost, "/api/party/{slot}", "owner", nullptr,
                                    nullptr).code == kQolErrUnsupported);

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

}  // namespace

int main() {
    test_template_and_query();
    test_owner_unregister_waits();
    test_inactive_and_missing_handler();
    std::printf("http_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
