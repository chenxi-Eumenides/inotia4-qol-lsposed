#include "host/http_dispatch.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

#if !defined(QOL_HTTP_DISPATCH_TEST_SEAMS)
#include "host/features/production.h"
#endif
#include "host/http_registry.h"
#include "infra/request_policy.h"

namespace qol::host {
namespace {
std::atomic<ResidentHttpRoute> g_resident_route{nullptr};
std::atomic<ResidentHttpDispatch> g_resident_dispatch{nullptr};

constexpr uint32_t kMaxHandlers = 128;
constexpr uint32_t kMaxPathParams = 16;

QolResult http_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

struct HandlerSlot {
    bool used = false;
    bool unregister_pending = false;
    bool binder_managed = false;
    bool binder_committed = false;
    contracts::HttpMethod method = contracts::HttpMethod::kGet;
    std::string path;
    std::string route_owner;
    std::string lifecycle_owner;
    std::array<std::string, kMaxPathParams> param_names{};
    uint32_t param_count = 0;
    // 注册时从路由表快照；dispatch 据此在进入 handler 前做 OP 门禁，运行期不再查路由表。
    uint8_t op_gated = 0;
    contracts::HttpQueryHandlerFn query = nullptr;
    contracts::HttpCommandHandlerFn command = nullptr;
    void* ctx = nullptr;
    uint32_t inflight = 0;
};

HandlerSlot g_handlers[kMaxHandlers];
std::mutex g_dispatch_mutex;

#if defined(QOL_HTTP_DISPATCH_TEST_SEAMS)
using OpAuthorizer = bool (*)();
OpAuthorizer g_test_op_authorizer = nullptr;

bool allow_op_for_test() { return true; }
bool deny_op_for_test() { return false; }
#endif

bool valid_method(contracts::HttpMethod method) {
    return method == contracts::HttpMethod::kGet || method == contracts::HttpMethod::kPost;
}

bool next_segment(const char* text, uint32_t length, uint32_t* cursor, uint32_t* start,
                 uint32_t* segment_length) {
    if (*cursor >= length) return false;
    *start = *cursor;
    while (*cursor < length && text[*cursor] != '/') ++*cursor;
    *segment_length = *cursor - *start;
    if (*cursor < length) ++*cursor;
    return true;
}

bool dynamic_segment(const std::string& segment, std::string* name) {
    if (segment.size() < 3 || segment.front() != '{' || segment.back() != '}') return false;
    const std::string candidate = segment.substr(1, segment.size() - 2);
    if (candidate.empty() || candidate.find('{') != std::string::npos ||
        candidate.find('}') != std::string::npos) {
        return false;
    }
    *name = candidate;
    return true;
}

bool validate_template(const char* path, uint32_t* out_param_count,
                       std::array<std::string, kMaxPathParams>* out_names) {
    if (path == nullptr || path[0] != '/') return false;
    const uint32_t length = static_cast<uint32_t>(std::strlen(path));
    if (length > 1 && path[length - 1] == '/') return false;
    uint32_t cursor = 1;
    uint32_t count = 0;
    if (length == 1) {
        *out_param_count = 0;
        return true;
    }
    while (cursor < length) {
        uint32_t start = 0;
        uint32_t segment_length = 0;
        if (!next_segment(path, length, &cursor, &start, &segment_length) || segment_length == 0) {
            return false;
        }
        const std::string segment(path + start, segment_length);
        if (!segment.empty() && segment.front() == '{') {
            if (count >= kMaxPathParams || !dynamic_segment(segment, &(*out_names)[count])) {
                return false;
            }
            ++count;
        } else if (segment.find('{') != std::string::npos || segment.find('}') != std::string::npos) {
            return false;
        }
    }
    *out_param_count = count;
    return true;
}

bool match_template(const HandlerSlot& slot, const contracts::HttpReq& request,
                    std::array<contracts::HttpPathParam, kMaxPathParams>* out_params) {
    if (request.path == nullptr || request.path_len == 0) return false;
    if (request.path[0] != '/') return false;
    if (request.path_len > 1 && request.path[request.path_len - 1] == '/') return false;
    uint32_t path_cursor = 1;
    uint32_t template_cursor = 1;
    uint32_t param_index = 0;
    while (template_cursor < slot.path.size() || path_cursor < request.path_len) {
        uint32_t template_start = 0;
        uint32_t template_length = 0;
        uint32_t path_start = 0;
        uint32_t path_length = 0;
        const bool has_template_segment = next_segment(slot.path.data(),
                                                       static_cast<uint32_t>(slot.path.size()),
                                                       &template_cursor, &template_start,
                                                       &template_length);
        const bool has_path_segment = next_segment(request.path, request.path_len, &path_cursor,
                                                   &path_start, &path_length);
        if (has_template_segment != has_path_segment) return false;
        if (!has_template_segment) break;
        const std::string_view template_segment(slot.path.data() + template_start, template_length);
        if (template_segment.size() >= 3 && template_segment.front() == '{' &&
            template_segment.back() == '}') {
            if (param_index >= slot.param_count) return false;
            (*out_params)[param_index] = {slot.param_names[param_index].data(),
                                          static_cast<uint32_t>(slot.param_names[param_index].size()),
                                          request.path + path_start, path_length};
            ++param_index;
        } else if (template_length != path_length ||
                   std::strncmp(slot.path.data() + template_start, request.path + path_start,
                                template_length) != 0) {
            return false;
        }
    }
    return param_index == slot.param_count;
}

void write_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    static constexpr char kPrefix[] = "{\"ok\":false,\"error\":\"";
    static constexpr char kSuffix[] = "\"}";
    const std::string body = std::string(kPrefix) + message + kSuffix;
    response->status = status;
    response->body_size = 0;
    if (response->body == nullptr || response->body_capacity < body.size()) return;
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
}

// query（GET）与 command（POST）注册共用。两者 handler 的函数指针类型相同（都是
// QolResult(*)(const HttpReq*, HttpRes*, void*)），不能用类型区分，故显式传入 command_slot
// 决定写入哪个槽，避免把命令注册进只读 query 槽。
QolResult register_handler(contracts::HttpMethod required_method, bool command_slot,
                           const char* invalid_message, contracts::HttpMethod method,
                           const char* path, const char* route_owner,
                           const char* lifecycle_owner, bool pending,
                           contracts::HttpQueryHandlerFn handler, void* ctx) {
    if (method != required_method || path == nullptr || route_owner == nullptr ||
        route_owner[0] == '\0' || lifecycle_owner == nullptr || lifecycle_owner[0] == '\0' ||
        handler == nullptr) {
        return http_error(kQolErrInvalidArgument, invalid_message);
    }

    std::array<std::string, kMaxPathParams> names{};
    uint32_t param_count = 0;
    if (!validate_template(path, &param_count, &names)) {
        return http_error(kQolErrInvalidArgument, "invalid route template");
    }
    const contracts::HttpRoute* route = nullptr;
    const QolResult lookup = http_registry_api().find(method, path, &route);
    if (!contracts::succeeded(lookup) || route == nullptr) {
        return http_error(kQolErrNotFound, "route not found");
    }
    if (route->owner == nullptr || std::strcmp(route->owner, route_owner) != 0) {
        return http_error(kQolErrConflict, "route owner mismatch");
    }

    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (HandlerSlot& slot : g_handlers) {
        if (slot.used && slot.method == method && slot.path == path) {
            if (slot.unregister_pending && slot.lifecycle_owner == lifecycle_owner &&
                slot.inflight == 0) {
                slot = HandlerSlot{};
                break;
            }
            return http_error(kQolErrConflict, "duplicate http handler");
        }
    }
    for (HandlerSlot& slot : g_handlers) {
        if (!slot.used) {
            slot.used = true;
            slot.unregister_pending = false;
            slot.method = method;
            slot.path = path;
            slot.route_owner = route_owner;
            slot.lifecycle_owner = lifecycle_owner;
            slot.param_names = names;
            slot.param_count = param_count;
            slot.op_gated = route->op_gated;
            slot.unregister_pending = pending;
            if (command_slot) {
                slot.query = nullptr;
                slot.command = handler;
            } else {
                slot.query = handler;
                slot.command = nullptr;
            }
            slot.ctx = ctx;
            slot.inflight = 0;
            slot.binder_managed = pending;
            slot.binder_committed = !pending;
            return contracts::ok();
        }
    }
    return http_error(kQolErrBlocked, "http handler capacity exhausted");
}

QolResult register_query(contracts::HttpMethod method, const char* path, const char* owner,
                         contracts::HttpQueryHandlerFn handler, void* ctx) {
    return register_handler(contracts::HttpMethod::kGet, false, "invalid query handler", method,
                            path, owner, owner, false, handler, ctx);
}

QolResult register_command(contracts::HttpMethod method, const char* path, const char* owner,
                           contracts::HttpCommandHandlerFn handler, void* ctx) {
    return register_handler(contracts::HttpMethod::kPost, true, "invalid command handler", method,
                            path, owner, owner, false, handler, ctx);
}

QolResult unregister_owner(const char* owner) {
    if (owner == nullptr || owner[0] == '\0') {
        return http_error(kQolErrInvalidArgument, "invalid handler owner");
    }
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    bool found = false;
    bool inflight = false;
    for (HandlerSlot& slot : g_handlers) {
        if (!slot.used || slot.route_owner != owner) continue;
        slot.unregister_pending = true;
        found = true;
        inflight = inflight || slot.inflight != 0;
    }
    if (!found) return http_error(kQolErrNotFound, "handler owner not found");
    if (inflight) return http_error(kQolErrBusy, "http handlers still active");
    for (HandlerSlot& slot : g_handlers) {
        if (!slot.used || slot.route_owner != owner) continue;
        slot.query = nullptr;
        slot.command = nullptr;
        slot.ctx = nullptr;
    }
    return contracts::ok();
}

QolResult dispatch(const contracts::HttpReq* request, contracts::HttpRes* response) {
    if (request == nullptr || response == nullptr || !valid_method(request->method) ||
        request->path == nullptr || request->path_len == 0 || request->path[0] != '/') {
        if (response != nullptr) write_error(response, 400, "bad request");
        return http_error(kQolErrInvalidArgument, "invalid http request");
    }
    response->body_size = 0;
    response->status = 0;
    const auto resident_route = g_resident_route.load(std::memory_order_acquire);
    const auto resident_dispatch = g_resident_dispatch.load(std::memory_order_acquire);
    if (resident_route && resident_dispatch && resident_route(*request)) return resident_dispatch(*request, *response);
    HttpRouteMatch route_match{};
    const QolResult route_match_result = http_registry_find_match(
        request->method, request->path, request->path_len, &route_match);
    if (contracts::succeeded(route_match_result) && route_match.resident) {
        write_error(response, 501, "not implemented");
        return http_error(kQolErrUnsupported, "resident route has no native handler");
    }
    if (!host_http_registry_accepting()) {
        write_error(response, 503, "http registry inactive");
        return http_error(kQolErrNotReady, "http registry inactive");
    }

    HandlerSlot* selected = nullptr;
#if defined(QOL_HTTP_DISPATCH_TEST_SEAMS)
    OpAuthorizer op_authorizer = nullptr;
#endif
    std::array<contracts::HttpPathParam, kMaxPathParams> params{};
    {
        std::lock_guard<std::mutex> lock(g_dispatch_mutex);
#if defined(QOL_HTTP_DISPATCH_TEST_SEAMS)
        op_authorizer = g_test_op_authorizer;
#endif
        for (HandlerSlot& slot : g_handlers) {
            if (slot.used && slot.method == request->method &&
                match_template(slot, *request, &params)) {
                if (slot.unregister_pending) {
                    write_error(response, 503, "http owner inactive");
                    return http_error(kQolErrNotReady, "http owner inactive");
                }
                selected = &slot;
                ++slot.inflight;
                break;
            }
        }
    }
    if (selected == nullptr) {
        route_match = {};
        const QolResult route_result = http_registry_find_match(
            request->method, request->path, request->path_len, &route_match);
        if (!contracts::succeeded(route_result)) {
            write_error(response, 404, "http handler not found");
            return http_error(kQolErrNotFound, "http handler not found");
        }
        if (route_match.unregister_pending ||
            (!route_match.resident && !host_http_registry_accepting())) {
            write_error(response, 503, "http owner inactive");
            return http_error(kQolErrNotReady, "http owner inactive");
        }
        if (route_match.binder_managed || route_match.resident) {
            write_error(response, 501, "not implemented");
            return http_error(kQolErrUnsupported, "http handler not implemented");
        }
        write_error(response, 404, "http handler not found");
        return http_error(kQolErrNotFound, "http handler not found");
    }

    // API listener state controls transport only. OP authorization is queried independently
    // and outside g_dispatch_mutex; an unavailable Host security query fails closed.
    bool op_enabled = false;
    if (selected->op_gated != 0) {
#if defined(QOL_HTTP_DISPATCH_TEST_SEAMS)
        op_enabled = op_authorizer != nullptr && op_authorizer();
#else
        op_enabled = security_op_enabled();
#endif
    }
    if (selected->op_gated != 0 && !op_enabled) {
        {
            std::lock_guard<std::mutex> lock(g_dispatch_mutex);
            --selected->inflight;
        }
        write_error(response, 403, "op disabled");
        return http_error(kQolErrBlocked, "op disabled");
    }

    uint64_t lease = 0;
    const QolResult lease_result = http_registry_api().lease_enter(
        request->method, selected->path.c_str(), &lease);
    if (!contracts::succeeded(lease_result)) {
        {
            std::lock_guard<std::mutex> lock(g_dispatch_mutex);
            --selected->inflight;
        }
        if (lease_result.code == kQolErrNotReady) {
            write_error(response, 503, "http registry inactive");
        } else if (lease_result.code == kQolErrBusy) {
            write_error(response, 503, lease_result.message != nullptr
                                           ? lease_result.message
                                           : "http route lease capacity");
        } else if (lease_result.code == kQolErrNotFound) {
            write_error(response, 404, "http route not found");
        } else {
            // A blocked operation is rejected by the independent OP authorization check above.
            write_error(response, 500, "http route lease failed");
        }
        return lease_result;
    }

    contracts::HttpReq request_copy = *request;
    request_copy.path_params = params.data();
    request_copy.path_param_count = selected->param_count;
    // GET 走只读 query 槽，POST 走 command 槽；两槽由是否墓碑分别置空，选中即非空。
    QolResult handler_result = contracts::ok();
    {
        const infra::ScopedRequestPolicy request_policy(infra::RequestPolicy{
            selected->method == contracts::HttpMethod::kPost && selected->op_gated != 0});
        handler_result = selected->method == contracts::HttpMethod::kPost
                             ? selected->command(&request_copy, response, selected->ctx)
                             : selected->query(&request_copy, response, selected->ctx);
    }
    (void)http_registry_api().lease_leave(lease);
    {
        std::lock_guard<std::mutex> lock(g_dispatch_mutex);
        --selected->inflight;
    }
    if (!contracts::succeeded(handler_result)) {
        if (response->body_size == 0) write_error(response, 500, "http handler failed");
        return handler_result;
    }
    if (response->status == 0) {
        write_error(response, 500, "http handler returned no status");
        return http_error(kQolErrInternal, "http handler returned no status");
    }
    return contracts::ok();
}

}  // namespace

void http_dispatch_begin_unregister_owner(const char* owner) {
    if (!owner) return;
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (auto& slot : g_handlers) {
        if (slot.used && slot.lifecycle_owner == owner) slot.unregister_pending = true;
    }
}

bool http_dispatch_poll_unregister_owner(const char* owner) {
    if (!owner) return false;
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (const auto& slot : g_handlers) {
        if (slot.used && slot.lifecycle_owner == owner && slot.inflight != 0) return false;
    }
    for (auto& slot : g_handlers) {
        if (!slot.used || slot.lifecycle_owner != owner) continue;
        slot.query = nullptr;
        slot.command = nullptr;
        slot.ctx = nullptr;
        slot.unregister_pending = true;
    }
    return true;
}

bool http_dispatch_poll_all_inflight() {
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (const HandlerSlot& slot : g_handlers) {
        if (slot.used && slot.inflight != 0) return false;
    }
    return true;
}

void http_dispatch_set_owner_ready(const char* owner) {
    if (owner == nullptr || owner[0] == '\0') return;
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (HandlerSlot& slot : g_handlers) {
        if (slot.used && slot.lifecycle_owner == owner && slot.binder_managed) {
            slot.unregister_pending = false;
            slot.binder_committed = true;
        }
    }
}

bool http_dispatch_rollback_uncommitted_owner(const char* owner) {
    if (owner == nullptr || owner[0] == '\0') return false;
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (const HandlerSlot& slot : g_handlers) {
        if (slot.used && slot.lifecycle_owner == owner && slot.binder_managed &&
            !slot.binder_committed && slot.inflight != 0) return false;
    }
    for (HandlerSlot& slot : g_handlers) {
        if (slot.used && slot.lifecycle_owner == owner && slot.binder_managed &&
            !slot.binder_committed) slot = HandlerSlot{};
    }
    return true;
}

contracts::QolHttpDispatchApi http_dispatch_api() {
    return {&register_query, &register_command, &unregister_owner, &dispatch};
}
void http_dispatch_bind_resident_control(ResidentHttpRoute route, ResidentHttpDispatch dispatch) {
    g_resident_dispatch.store(dispatch, std::memory_order_release);
    g_resident_route.store(route, std::memory_order_release);
}

QolResult http_dispatch_dynamic(const contracts::HttpReq* request,
                                std::vector<uint8_t>* response_body, uint32_t max_capacity,
                                uint16_t* out_status, uint32_t* out_body_size) {
    if (request == nullptr || response_body == nullptr || out_status == nullptr ||
        out_body_size == nullptr || max_capacity < 128) {
        return contracts::error(kQolErrInvalidArgument, "invalid dynamic http response", 30);
    }
    constexpr uint32_t kInitialCapacity = 262144;
    constexpr uint32_t kGrowthSlack = 4096;
    try {
        response_body->assign(std::min(kInitialCapacity, max_capacity), 0);
    } catch (const std::bad_alloc&) {
        *out_status = 500;
        *out_body_size = 0;
        return contracts::error(kQolErrInternal, "http response allocation failed", 31);
    }
    contracts::HttpRes response{response_body->data(), static_cast<uint32_t>(response_body->size()),
                                0, 0};
    const auto dispatch_safely = [request](contracts::HttpRes* out) {
        try {
            return dispatch(request, out);
        } catch (const std::bad_alloc&) {
            constexpr char kAllocationFailed[] = "{\"ok\":false,\"error\":\"oom\"}";
            out->status = 500;
            out->body_size = sizeof(kAllocationFailed) - 1;
            if (out->body != nullptr && out->body_capacity >= out->body_size)
                std::memcpy(out->body, kAllocationFailed, out->body_size);
            return contracts::error(kQolErrInternal, "http handler allocation failed", 30);
        }
    };
    QolResult result = dispatch_safely(&response);
    while (request->method == contracts::HttpMethod::kGet &&
           response.body_size > response.body_capacity) {
        const uint32_t required = response.body_size;
        if (required > max_capacity) {
            constexpr char kTooLarge[] = "{\"ok\":false,\"error\":\"response too large\"}";
            response_body->assign(kTooLarge, kTooLarge + sizeof(kTooLarge) - 1);
            response.status = 500;
            response.body_size = sizeof(kTooLarge) - 1;
            result = contracts::ok();
            break;
        } else {
            const uint32_t slack = std::min(kGrowthSlack, max_capacity);
            const uint32_t target = required > max_capacity - slack ? max_capacity : required + slack;
            try {
                response_body->resize(target);
            } catch (const std::bad_alloc&) {
                constexpr char kAllocationFailed[] = "{\"ok\":false,\"error\":\"oom\"}";
                std::memcpy(response_body->data(), kAllocationFailed,
                            sizeof(kAllocationFailed) - 1);
                response.status = 500;
                response.body_size = sizeof(kAllocationFailed) - 1;
                *out_status = response.status;
                *out_body_size = response.body_size;
                return contracts::ok();
            }
            response = {response_body->data(), static_cast<uint32_t>(response_body->size()), 0, 0};
            result = dispatch_safely(&response);
            if (response.body_size > response.body_capacity) {
                constexpr char kTooLarge[] = "{\"ok\":false,\"error\":\"response too large\"}";
                response_body->assign(kTooLarge, kTooLarge + sizeof(kTooLarge) - 1);
                response.status = 500;
                response.body_size = sizeof(kTooLarge) - 1;
                result = contracts::ok();
                break;
            }
        }
    }
    *out_status = response.status;
    *out_body_size = response.body_size;
    return result;
}

QolResult http_dispatch_register_queries(const contracts::HttpQueryHandlerDescriptor* handlers,
                                         uint32_t count) {
    if (count > 0 && handlers == nullptr) return http_error(kQolErrInvalidArgument, "invalid handlers");
    for (uint32_t i = 0; i < count; ++i) {
        const contracts::HttpQueryHandlerDescriptor& handler = handlers[i];
        // descriptor 数组同型承载 GET/POST 两类条目：按 method 分流到 query/command 槽。
        const QolResult result =
            handler.method == contracts::HttpMethod::kPost
                ? register_command(handler.method, handler.path, handler.owner, handler.handler,
                                   handler.ctx)
                : register_query(handler.method, handler.path, handler.owner, handler.handler,
                                 handler.ctx);
        if (!contracts::succeeded(result)) {
            if (handler.owner != nullptr) (void)unregister_owner(handler.owner);
            return result;
        }
    }
    return contracts::ok();
}

QolResult http_dispatch_register_query_for_owner(
    const char* lifecycle_owner, contracts::HttpMethod method, const char* path,
    const char* route_owner, contracts::HttpQueryHandlerFn handler, void* ctx) {
    return register_handler(contracts::HttpMethod::kGet, false, "invalid query handler", method,
                            path, route_owner, lifecycle_owner, true, handler, ctx);
}

QolResult http_dispatch_register_command_for_owner(
    const char* lifecycle_owner, contracts::HttpMethod method, const char* path,
    const char* route_owner, contracts::HttpCommandHandlerFn handler, void* ctx) {
    return register_handler(contracts::HttpMethod::kPost, true, "invalid command handler", method,
                            path, route_owner, lifecycle_owner, true, handler, ctx);
}

QolResult http_dispatch_register_builtin_query(contracts::HttpMethod method, const char* path,
                                               const char* owner,
                                               contracts::HttpQueryHandlerFn handler, void* ctx) {
    return register_query(method, path, owner, handler, ctx);
}

void http_dispatch_set_api_enabled(bool enabled) {
    // Retained for API/listener compatibility. Dispatch permission is not an API transport gate.
    (void)enabled;
}

#if defined(QOL_HTTP_DISPATCH_TEST_SEAMS)
void http_dispatch_set_op_authorizer_for_test(bool (*authorizer)()) {
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    g_test_op_authorizer = authorizer;
}

void http_dispatch_set_op_enabled(bool enabled) {
    http_dispatch_set_op_authorizer_for_test(enabled ? &allow_op_for_test : &deny_op_for_test);
}
#endif

void http_dispatch_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (HandlerSlot& slot : g_handlers) slot = HandlerSlot{};
#if defined(QOL_HTTP_DISPATCH_TEST_SEAMS)
    g_test_op_authorizer = nullptr;
#endif
}

}  // namespace qol::host
