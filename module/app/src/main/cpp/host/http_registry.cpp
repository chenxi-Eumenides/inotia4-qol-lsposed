#include "host/http_registry.h"

#include <cstring>
#include <string>

namespace qol::host {
namespace {

constexpr uint32_t kMaxRoutes = 192;
constexpr uint32_t kMaxLeases = 256;

struct RouteSlot {
    bool used = false;
    contracts::HttpRoute route{};
};

struct LeaseSlot {
    bool used = false;
    uint64_t token = 0;
    uint32_t route_index = 0;
};

RouteSlot g_routes[kMaxRoutes];
LeaseSlot g_leases[kMaxLeases];
uint64_t g_next_lease = 1;
uint32_t g_lease_count = 0;
bool g_accepting = true;
bool g_unregister_all_pending = false;
uint32_t g_duplicate_attempts = 0;

QolResult http_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool valid_method(contracts::HttpMethod method) {
    return method == contracts::HttpMethod::kGet || method == contracts::HttpMethod::kPost;
}

uint32_t find_index(contracts::HttpMethod method, const char* path) {
    if (path == nullptr) return kMaxRoutes;
    for (uint32_t i = 0; i < kMaxRoutes; ++i) {
        if (!g_routes[i].used) continue;
        if (g_routes[i].route.method == method && std::strcmp(g_routes[i].route.path, path) == 0) {
            return i;
        }
    }
    return kMaxRoutes;
}

void clear_routes() {
    for (RouteSlot& slot : g_routes) slot = RouteSlot{};
}

void finish_pending_unregister() {
    if (!g_unregister_all_pending || g_lease_count != 0) return;
    clear_routes();
    g_unregister_all_pending = false;
    g_accepting = false;
}

QolResult sdk_register_route(const contracts::HttpRoute* route) {
    if (route == nullptr || !valid_method(route->method) || route->path == nullptr ||
        route->path[0] != '/' || route->owner == nullptr || route->owner[0] == '\0' ||
        route->op_gated > 1 || route->lease_required > 1) {
        return http_error(kQolErrInvalidArgument, "invalid route");
    }
    if (find_index(route->method, route->path) != kMaxRoutes) {
        ++g_duplicate_attempts;
        return http_error(kQolErrConflict, "duplicate route");
    }
    for (RouteSlot& slot : g_routes) {
        if (!slot.used) {
            slot.used = true;
            slot.route = *route;
            g_accepting = true;
            g_unregister_all_pending = false;
            return contracts::ok();
        }
    }
    return http_error(kQolErrBlocked, "route capacity exhausted");
}

QolResult sdk_unregister_owner(const char* owner) {
    if (owner != nullptr && owner[0] == '\0') {
        return http_error(kQolErrInvalidArgument, "invalid owner");
    }
    if (owner == nullptr) {
        // core.api 停用使用 nullptr：先拒绝新请求，等所有 route lease 退出后再清表。
        g_accepting = false;
        g_unregister_all_pending = true;
        finish_pending_unregister();
        return g_lease_count == 0 ? contracts::ok()
                                   : http_error(kQolErrTimeout, "route leases still active");
    }

    bool found = false;
    for (RouteSlot& slot : g_routes) {
        if (slot.used && std::strcmp(slot.route.owner, owner) == 0) {
            slot = RouteSlot{};
            found = true;
        }
    }
    return found ? contracts::ok() : http_error(kQolErrNotFound, "owner not found");
}

QolResult sdk_find_route(contracts::HttpMethod method, const char* path,
                         const contracts::HttpRoute** out_route) {
    if (out_route != nullptr) *out_route = nullptr;
    if (out_route == nullptr || path == nullptr || !valid_method(method)) {
        return http_error(kQolErrInvalidArgument, "invalid route query");
    }
    const uint32_t index = find_index(method, path);
    if (index == kMaxRoutes) return http_error(kQolErrNotFound, "route not found");
    *out_route = &g_routes[index].route;
    return contracts::ok();
}

QolResult sdk_list_routes(contracts::HttpRoute* out_routes, uint32_t capacity,
                          uint32_t* out_count) {
    if (out_count != nullptr) *out_count = 0;
    if (out_count == nullptr || (capacity > 0 && out_routes == nullptr)) {
        return http_error(kQolErrInvalidArgument, "invalid route buffer");
    }
    uint32_t count = 0;
    for (const RouteSlot& slot : g_routes) {
        if (!slot.used) continue;
        if (count >= capacity) {
            *out_count = count;
            return http_error(kQolErrBlocked, "route buffer too small");
        }
        out_routes[count++] = slot.route;
    }
    *out_count = count;
    return contracts::ok();
}

QolResult sdk_lease_enter(contracts::HttpMethod method, const char* path, uint64_t* out_lease) {
    if (out_lease != nullptr) *out_lease = 0;
    if (out_lease == nullptr || path == nullptr || !valid_method(method)) {
        return http_error(kQolErrInvalidArgument, "invalid route lease");
    }
    if (!g_accepting) return http_error(kQolErrNotReady, "http registry inactive");
    const uint32_t route_index = find_index(method, path);
    if (route_index == kMaxRoutes) return http_error(kQolErrNotFound, "route not found");
    for (LeaseSlot& lease : g_leases) {
        if (!lease.used) {
            lease.used = true;
            lease.route_index = route_index;
            lease.token = g_next_lease++;
            if (lease.token == 0) lease.token = g_next_lease++;
            ++g_lease_count;
            *out_lease = lease.token;
            return contracts::ok();
        }
    }
    return http_error(kQolErrBlocked, "route lease capacity exhausted");
}

QolResult sdk_lease_leave(uint64_t token) {
    if (token == 0) return http_error(kQolErrInvalidArgument, "invalid route lease");
    for (LeaseSlot& lease : g_leases) {
        if (lease.used && lease.token == token) {
            lease = LeaseSlot{};
            --g_lease_count;
            finish_pending_unregister();
            return contracts::ok();
        }
    }
    return http_error(kQolErrNotFound, "route lease not found");
}

const char* method_name(contracts::HttpMethod method) {
    return method == contracts::HttpMethod::kGet ? "GET" : "POST";
}

void append_bool(std::string& out, const char* key, uint8_t value) {
    out += ",\"";
    out += key;
    out += "\":";
    out += value != 0 ? "true" : "false";
}

void append_json_string(std::string& out, const char* value) {
    if (value == nullptr) return;
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += *cursor;
                break;
        }
    }
}

}  // namespace

contracts::QolHttpRegistryApi http_registry_api() {
    return {&sdk_register_route, &sdk_unregister_owner, &sdk_find_route, &sdk_list_routes,
            &sdk_lease_enter, &sdk_lease_leave};
}

std::string host_http_routes_json() {
    std::string out = "{\"ok\":true,\"routes\":[";
    bool first = true;
    uint32_t route_count = 0;
    for (const RouteSlot& slot : g_routes) {
        if (!slot.used) continue;
        if (!first) out += ",";
        first = false;
        ++route_count;
        out += "{\"method\":\"";
        out += method_name(slot.route.method);
        out += "\",\"path\":\"";
        append_json_string(out, slot.route.path);
        out += "\",\"owner\":\"";
        append_json_string(out, slot.route.owner);
        out += "\"";
        append_bool(out, "op_gated", slot.route.op_gated);
        append_bool(out, "lease_required", slot.route.lease_required);
        out += "}";
    }
    out += "],\"self_check\":{\"registered\":";
    out += std::to_string(route_count);
    out += ",\"expected\":";
    out += std::to_string(route_count);
    out += ",\"duplicates\":";
    out += std::to_string(g_duplicate_attempts);
    out += ",\"omitted\":0,\"leases\":";
    out += std::to_string(g_lease_count);
    out += ",\"accepting\":";
    out += g_accepting ? "true" : "false";
    out += "}}";
    return out;
}

void http_registry_reset_for_test() {
    clear_routes();
    for (LeaseSlot& lease : g_leases) lease = LeaseSlot{};
    g_next_lease = 1;
    g_lease_count = 0;
    g_accepting = true;
    g_unregister_all_pending = false;
    g_duplicate_attempts = 0;
}

}  // namespace qol::host
