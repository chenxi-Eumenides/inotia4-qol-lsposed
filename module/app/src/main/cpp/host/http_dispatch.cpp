#include "host/http_dispatch.h"

#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "host/http_registry.h"

namespace qol::host {
namespace {

constexpr uint32_t kMaxHandlers = 128;
constexpr uint32_t kMaxPathParams = 16;

QolResult http_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

struct HandlerSlot {
    bool used = false;
    bool unregister_pending = false;
    contracts::HttpMethod method = contracts::HttpMethod::kGet;
    std::string path;
    std::string owner;
    std::array<std::string, kMaxPathParams> param_names{};
    uint32_t param_count = 0;
    contracts::HttpQueryHandlerFn query = nullptr;
    void* ctx = nullptr;
    uint32_t inflight = 0;
};

HandlerSlot g_handlers[kMaxHandlers];
std::mutex g_dispatch_mutex;
std::condition_variable g_dispatch_cv;

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

QolResult register_query(contracts::HttpMethod method, const char* path, const char* owner,
                         contracts::HttpQueryHandlerFn handler, void* ctx) {
    if (method != contracts::HttpMethod::kGet || path == nullptr || owner == nullptr ||
        owner[0] == '\0' || handler == nullptr) {
        return http_error(kQolErrInvalidArgument, "invalid query handler");
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
    if (route->owner == nullptr || std::strcmp(route->owner, owner) != 0) {
        return http_error(kQolErrConflict, "route owner mismatch");
    }

    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (const HandlerSlot& slot : g_handlers) {
        if (slot.used && slot.method == method && slot.path == path) {
            return http_error(kQolErrConflict, "duplicate http handler");
        }
    }
    for (HandlerSlot& slot : g_handlers) {
        if (!slot.used) {
            slot.used = true;
            slot.unregister_pending = false;
            slot.method = method;
            slot.path = path;
            slot.owner = owner;
            slot.param_names = names;
            slot.param_count = param_count;
            slot.query = handler;
            slot.ctx = ctx;
            slot.inflight = 0;
            return contracts::ok();
        }
    }
    return http_error(kQolErrBlocked, "http handler capacity exhausted");
}

QolResult register_command(contracts::HttpMethod, const char*, const char*,
                           contracts::HttpCommandHandlerFn, void*) {
    return http_error(kQolErrUnsupported, "command dispatch reserved");
}

QolResult unregister_owner(const char* owner) {
    if (owner == nullptr || owner[0] == '\0') {
        return http_error(kQolErrInvalidArgument, "invalid handler owner");
    }
    {
        std::lock_guard<std::mutex> lock(g_dispatch_mutex);
        bool found = false;
        for (HandlerSlot& slot : g_handlers) {
            if (slot.used && slot.owner == owner) {
                slot.unregister_pending = true;
                found = true;
            }
        }
        if (!found) return http_error(kQolErrNotFound, "handler owner not found");
    }

    const QolResult route_result = http_registry_api().unregister_owner(owner);
    std::unique_lock<std::mutex> lock(g_dispatch_mutex);
    g_dispatch_cv.wait(lock, [owner] {
        for (const HandlerSlot& slot : g_handlers) {
            if (slot.used && slot.owner == owner && slot.inflight != 0) return false;
        }
        return true;
    });
    for (HandlerSlot& slot : g_handlers) {
        if (slot.used && slot.owner == owner) slot = HandlerSlot{};
    }
    return route_result;
}

QolResult dispatch(const contracts::HttpReq* request, contracts::HttpRes* response) {
    if (request == nullptr || response == nullptr || !valid_method(request->method) ||
        request->path == nullptr || request->path_len == 0 || request->path[0] != '/') {
        if (response != nullptr) write_error(response, 400, "bad request");
        return http_error(kQolErrInvalidArgument, "invalid http request");
    }
    response->body_size = 0;
    response->status = 0;
    if (!host_http_registry_accepting()) {
        write_error(response, 503, "http registry inactive");
        return http_error(kQolErrNotReady, "http registry inactive");
    }

    HandlerSlot* selected = nullptr;
    std::array<contracts::HttpPathParam, kMaxPathParams> params{};
    {
        std::lock_guard<std::mutex> lock(g_dispatch_mutex);
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
        write_error(response, 404, "http handler not found");
        return http_error(kQolErrNotFound, "http handler not found");
    }

    uint64_t lease = 0;
    const QolResult lease_result = http_registry_api().lease_enter(
        request->method, selected->path.c_str(), &lease);
    if (!contracts::succeeded(lease_result)) {
        {
            std::lock_guard<std::mutex> lock(g_dispatch_mutex);
            --selected->inflight;
            g_dispatch_cv.notify_all();
        }
        if (lease_result.code == kQolErrNotReady) {
            write_error(response, 503, "http registry inactive");
        } else {
            write_error(response, 404, "http route not found");
        }
        return lease_result;
    }

    contracts::HttpReq request_copy = *request;
    request_copy.path_params = params.data();
    request_copy.path_param_count = selected->param_count;
    const QolResult handler_result = selected->query(&request_copy, response, selected->ctx);
    (void)http_registry_api().lease_leave(lease);
    {
        std::lock_guard<std::mutex> lock(g_dispatch_mutex);
        --selected->inflight;
        g_dispatch_cv.notify_all();
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

contracts::QolHttpDispatchApi http_dispatch_api() {
    return {&register_query, &register_command, &unregister_owner, &dispatch};
}

QolResult http_dispatch_register_builtin_query(contracts::HttpMethod method, const char* path,
                                               const char* owner,
                                               contracts::HttpQueryHandlerFn handler, void* ctx) {
    return register_query(method, path, owner, handler, ctx);
}

void http_dispatch_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_dispatch_mutex);
    for (HandlerSlot& slot : g_handlers) slot = HandlerSlot{};
}

}  // namespace qol::host
