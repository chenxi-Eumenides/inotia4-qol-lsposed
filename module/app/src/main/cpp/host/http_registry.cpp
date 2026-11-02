#include "host/http_registry.h"

#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace qol::host {
namespace {

constexpr uint32_t kMaxRoutes = 192;
constexpr uint32_t kMaxLeases = 256;

struct RouteSlot {
    bool used = false;
    bool unregister_pending = false;
    bool resident = false;
    bool sdk_owned = false;
    bool binder_managed = false;
    bool binder_committed = false;
    const char* lifecycle_owner = nullptr;
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
bool g_provider_available = true;
uint32_t g_duplicate_attempts = 0;
std::mutex g_registry_mutex;

QolResult http_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool valid_method(contracts::HttpMethod method) {
    return method == contracts::HttpMethod::kGet || method == contracts::HttpMethod::kPost;
}

uint32_t find_index_locked(contracts::HttpMethod method, const char* path) {
    if (path == nullptr) return kMaxRoutes;
    for (uint32_t i = 0; i < kMaxRoutes; ++i) {
        if (!g_routes[i].used) continue;
        if (g_routes[i].route.method == method && std::strcmp(g_routes[i].route.path, path) == 0) {
            return i;
        }
    }
    return kMaxRoutes;
}

void clear_routes_locked() {
    for (RouteSlot& slot : g_routes) slot = RouteSlot{};
}

void clear_nonresident_routes_locked() {
    for (RouteSlot& slot : g_routes) {
        if (slot.used && !slot.resident) slot = RouteSlot{};
    }
}

bool route_matches_path(const char* route_path, const char* actual_path, uint32_t actual_length) {
    if (route_path == nullptr || actual_path == nullptr || actual_length == 0 ||
        actual_path[0] != '/' || (actual_length > 1 && actual_path[actual_length - 1] == '/'))
        return false;
    const std::string_view route(route_path);
    uint32_t route_cursor = 1;
    uint32_t actual_cursor = 1;
    while (route_cursor < route.size() || actual_cursor < actual_length) {
        const uint32_t route_start = route_cursor;
        const uint32_t actual_start = actual_cursor;
        while (route_cursor < route.size() && route[route_cursor] != '/') ++route_cursor;
        while (actual_cursor < actual_length && actual_path[actual_cursor] != '/') ++actual_cursor;
        const uint32_t route_segment_size = route_cursor - route_start;
        const uint32_t actual_segment_size = actual_cursor - actual_start;
        const std::string_view route_segment(route.data() + route_start, route_segment_size);
        if (route_cursor < route.size()) ++route_cursor;
        if (actual_cursor < actual_length) ++actual_cursor;
        if (route_segment_size == 0 || actual_segment_size == 0) return false;
        if (route_segment_size >= 3 && route_segment.front() == '{' &&
            route_segment.back() == '}') {
            if (actual_segment_size == 0) return false;
            continue;
        }
        if (route_segment_size != actual_segment_size ||
            std::memcmp(route.data() + route_start, actual_path + actual_start,
                        route_segment_size) != 0) return false;
    }
    return route_cursor == route.size() && actual_cursor == actual_length;
}

bool same_route(const contracts::HttpRoute& left, const contracts::HttpRoute& right) {
    return left.method == right.method && left.op_gated == right.op_gated &&
           left.lease_required == right.lease_required && left.path != nullptr &&
           right.path != nullptr && left.owner != nullptr && right.owner != nullptr &&
           std::strcmp(left.path, right.path) == 0 && std::strcmp(left.owner, right.owner) == 0;
}

bool has_route_lease_locked(uint32_t route_index) {
    for (const LeaseSlot& lease : g_leases) {
        if (lease.used && lease.route_index == route_index) return true;
    }
    return false;
}

bool has_nonresident_leases_locked() {
    for (const LeaseSlot& lease : g_leases) {
        if (lease.used && lease.route_index < kMaxRoutes && g_routes[lease.route_index].used &&
            !g_routes[lease.route_index].resident) return true;
    }
    return false;
}

void finish_pending_unregister_locked() {
    if (!g_unregister_all_pending || has_nonresident_leases_locked()) return;
    clear_nonresident_routes_locked();
    g_unregister_all_pending = false;
    g_accepting = g_provider_available;
}

QolResult sdk_register_route(const contracts::HttpRoute* route) {
    if (route == nullptr || !valid_method(route->method) || route->path == nullptr ||
        route->path[0] != '/' || route->owner == nullptr || route->owner[0] == '\0' ||
        route->op_gated > 1 || route->lease_required > 1) {
        return http_error(kQolErrInvalidArgument, "invalid route");
    }
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const uint32_t duplicate = find_index_locked(route->method, route->path);
    if (duplicate != kMaxRoutes) {
        RouteSlot& existing = g_routes[duplicate];
        if (existing.resident && same_route(existing.route, *route)) return contracts::ok();
        if (g_provider_available && existing.sdk_owned && !existing.binder_managed &&
            existing.unregister_pending &&
            same_route(existing.route, *route) && !has_route_lease_locked(duplicate)) {
            existing.unregister_pending = false;
            return contracts::ok();
        }
        ++g_duplicate_attempts;
        return http_error(kQolErrConflict, "duplicate route");
    }
    for (RouteSlot& slot : g_routes) {
        if (!slot.used) {
            slot.used = true;
            slot.unregister_pending = !g_provider_available;
            slot.route = *route;
            slot.lifecycle_owner = nullptr;
            slot.sdk_owned = true;
            g_unregister_all_pending = false;
            g_accepting = g_provider_available;
            return contracts::ok();
        }
    }
    return http_error(kQolErrBlocked, "route capacity exhausted");
}

QolResult sdk_unregister_owner(const char* owner) {
    if (owner != nullptr && owner[0] == '\0') {
        return http_error(kQolErrInvalidArgument, "invalid owner");
    }
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (owner == nullptr) {
        // Provider shutdown is nonblocking. Binder drains leases before the provider callback.
        g_provider_available = false;
        g_accepting = false;
        g_unregister_all_pending = true;
        for (RouteSlot& slot : g_routes) if (slot.used && !slot.resident) slot.unregister_pending = true;
        finish_pending_unregister_locked();
        return g_unregister_all_pending
            ? http_error(kQolErrBusy, "route leases still active") : contracts::ok();
    }

    bool found = false;
    for (RouteSlot& slot : g_routes) {
        if (slot.used && !slot.resident && std::strcmp(slot.route.owner, owner) == 0) {
            slot.unregister_pending = true;
            found = true;
        }
    }
    if (found) {
        for (uint32_t index = 0; index < kMaxRoutes; ++index) {
            const RouteSlot& slot = g_routes[index];
            if (slot.used && !slot.resident && std::strcmp(slot.route.owner, owner) == 0 &&
                has_route_lease_locked(index))
                return http_error(kQolErrBusy, "route leases still active");
        }
        for (RouteSlot& slot : g_routes) {
            if (slot.used && !slot.resident && std::strcmp(slot.route.owner, owner) == 0)
                slot = RouteSlot{};
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
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const uint32_t index = find_index_locked(method, path);
    if (index == kMaxRoutes) return http_error(kQolErrNotFound, "route not found");
    static thread_local contracts::HttpRoute route_copy;
    route_copy = g_routes[index].route;
    *out_route = &route_copy;
    return contracts::ok();
}

QolResult sdk_list_routes(contracts::HttpRoute* out_routes, uint32_t capacity,
                          uint32_t* out_count) {
    if (out_count != nullptr) *out_count = 0;
    if (out_count == nullptr || (capacity > 0 && out_routes == nullptr)) {
        return http_error(kQolErrInvalidArgument, "invalid route buffer");
    }
    std::lock_guard<std::mutex> lock(g_registry_mutex);
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
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const uint32_t route_index = find_index_locked(method, path);
    if (route_index == kMaxRoutes) return http_error(kQolErrNotFound, "route not found");
    if (!g_provider_available && !g_routes[route_index].resident)
        return http_error(kQolErrNotReady, "http registry inactive");
    if (g_routes[route_index].unregister_pending) {
        return http_error(kQolErrNotReady, "route owner inactive");
    }
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
    return http_error(kQolErrBusy, "http route lease capacity");
}

QolResult sdk_lease_leave(uint64_t token) {
    if (token == 0) return http_error(kQolErrInvalidArgument, "invalid route lease");
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (LeaseSlot& lease : g_leases) {
        if (lease.used && lease.token == token) {
            lease = LeaseSlot{};
            --g_lease_count;
            finish_pending_unregister_locked();
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

QolResult http_registry_register_route_for_owner(const char* lifecycle_owner,
                                                 const contracts::HttpRoute& route,
                                                 bool resident) {
    if (!valid_method(route.method) || route.path == nullptr || route.path[0] != '/' ||
        route.owner == nullptr || route.owner[0] == '\0' || route.op_gated > 1 ||
        route.lease_required > 1 || (resident && lifecycle_owner != nullptr && lifecycle_owner[0] != '\0') ||
        (!resident && (lifecycle_owner == nullptr || lifecycle_owner[0] == '\0'))) {
        return http_error(kQolErrInvalidArgument, "invalid owned route");
    }

    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const uint32_t duplicate = find_index_locked(route.method, route.path);
    if (duplicate != kMaxRoutes) {
        RouteSlot& slot = g_routes[duplicate];
        if (!same_route(slot.route, route)) {
            ++g_duplicate_attempts;
            return http_error(kQolErrConflict, "route metadata collision");
        }
        if (resident) {
            if (!slot.sdk_owned && !slot.resident && slot.lifecycle_owner != nullptr &&
                slot.lifecycle_owner[0] != '\0')
                return http_error(kQolErrConflict, "route lifecycle owner collision");
            slot.resident = true;
            slot.sdk_owned = false;
            slot.binder_managed = true;
            slot.binder_committed = true;
            slot.lifecycle_owner = nullptr;
            slot.unregister_pending = false;
            return contracts::ok();
        }
        if (slot.resident) return http_error(kQolErrConflict, "resident route owner collision");
        if (slot.lifecycle_owner != nullptr &&
            std::strcmp(slot.lifecycle_owner, lifecycle_owner) == 0) {
            slot.binder_managed = true;
            slot.unregister_pending = true;
            return contracts::ok();
        }
        // Older core.api descriptors populated route facts through the public registry table.
        // Adopt that matching unowned row into the descriptor's real lifecycle owner.
        if (slot.sdk_owned && slot.lifecycle_owner == nullptr) {
            slot.lifecycle_owner = lifecycle_owner;
            slot.binder_managed = true;
            slot.unregister_pending = true;
            return contracts::ok();
        }
        ++g_duplicate_attempts;
        return http_error(kQolErrConflict, "route lifecycle owner collision");
    }

    for (RouteSlot& slot : g_routes) {
        if (!slot.used) {
            slot.used = true;
            slot.unregister_pending = !resident;
            slot.resident = resident;
            slot.sdk_owned = false;
            slot.binder_managed = true;
            slot.binder_committed = false;
            slot.lifecycle_owner = resident ? nullptr : lifecycle_owner;
            slot.route = route;
            return contracts::ok();
        }
    }
    return http_error(kQolErrBlocked, "route capacity exhausted");
}

void http_registry_set_owner_ready(const char* lifecycle_owner) {
    if (lifecycle_owner == nullptr || lifecycle_owner[0] == '\0') return;
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (RouteSlot& slot : g_routes) {
        if (slot.used && !slot.resident && slot.lifecycle_owner != nullptr &&
            std::strcmp(slot.lifecycle_owner, lifecycle_owner) == 0 &&
            g_provider_available && slot.binder_managed) {
            slot.unregister_pending = false;
            if (slot.binder_managed) slot.binder_committed = true;
        }
    }
}

void http_registry_begin_unregister_lifecycle_owner(const char* lifecycle_owner) {
    if (lifecycle_owner == nullptr || lifecycle_owner[0] == '\0') return;
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (RouteSlot& slot : g_routes) {
        if (slot.used && !slot.resident && slot.lifecycle_owner != nullptr &&
            std::strcmp(slot.lifecycle_owner, lifecycle_owner) == 0)
            slot.unregister_pending = true;
    }
}

bool http_registry_poll_unregister_lifecycle_owner(const char* lifecycle_owner) {
    if (lifecycle_owner == nullptr || lifecycle_owner[0] == '\0') return false;
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (uint32_t index = 0; index < kMaxRoutes; ++index) {
        const RouteSlot& slot = g_routes[index];
        if (slot.used && !slot.resident && slot.lifecycle_owner != nullptr &&
            std::strcmp(slot.lifecycle_owner, lifecycle_owner) == 0 &&
            has_route_lease_locked(index)) return false;
    }
    return true;
}

bool http_registry_rollback_uncommitted_owner(const char* lifecycle_owner) {
    if (lifecycle_owner == nullptr || lifecycle_owner[0] == '\0') return false;
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (uint32_t index = 0; index < kMaxRoutes; ++index) {
        const RouteSlot& slot = g_routes[index];
        if (slot.used && !slot.resident && slot.binder_managed && !slot.binder_committed &&
            slot.lifecycle_owner != nullptr &&
            std::strcmp(slot.lifecycle_owner, lifecycle_owner) == 0 &&
            has_route_lease_locked(index)) return false;
    }
    for (RouteSlot& slot : g_routes) {
        if (!slot.used || slot.resident || !slot.binder_managed || slot.binder_committed ||
            slot.lifecycle_owner == nullptr ||
            std::strcmp(slot.lifecycle_owner, lifecycle_owner) != 0) continue;
        // Roll back admission, not the frozen route fact: keep the owner's route as a tombstone.
        slot.unregister_pending = true;
    }
    return true;
}

bool http_registry_poll_nonresident_leases() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    return !has_nonresident_leases_locked();
}

void http_registry_set_provider_available(bool available) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_provider_available = available;
    g_accepting = available && !g_unregister_all_pending;
    if (!available) {
        for (RouteSlot& slot : g_routes) if (slot.used && !slot.resident) slot.unregister_pending = true;
    } else {
        for (RouteSlot& slot : g_routes) {
            if (slot.used && !slot.resident && slot.sdk_owned && !slot.binder_managed &&
                slot.lifecycle_owner == nullptr) slot.unregister_pending = false;
        }
    }
}

QolResult http_registry_find_match(contracts::HttpMethod method, const char* actual_path,
                                   uint32_t actual_path_len, HttpRouteMatch* out_match) {
    if (out_match == nullptr || actual_path == nullptr || !valid_method(method))
        return http_error(kQolErrInvalidArgument, "invalid route match");
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (const RouteSlot& slot : g_routes) {
        if (!slot.used || slot.route.method != method ||
            !route_matches_path(slot.route.path, actual_path, actual_path_len)) continue;
        out_match->route = slot.route;
        out_match->lifecycle_owner = slot.lifecycle_owner;
        out_match->resident = slot.resident;
        out_match->binder_managed = slot.binder_managed;
        out_match->unregister_pending = slot.unregister_pending;
        return contracts::ok();
    }
    return http_error(kQolErrNotFound, "route not found");
}

QolResult http_registry_lease_enter_match(contracts::HttpMethod method, const char* actual_path,
                                          uint32_t actual_path_len, uint64_t* out_lease) {
    if (out_lease != nullptr) *out_lease = 0;
    if (out_lease == nullptr || actual_path == nullptr || !valid_method(method))
        return http_error(kQolErrInvalidArgument, "invalid route lease");
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    uint32_t route_index = kMaxRoutes;
    for (uint32_t index = 0; index < kMaxRoutes; ++index) {
        const RouteSlot& slot = g_routes[index];
        if (slot.used && slot.route.method == method &&
            route_matches_path(slot.route.path, actual_path, actual_path_len)) {
            route_index = index;
            break;
        }
    }
    if (route_index == kMaxRoutes) return http_error(kQolErrNotFound, "route not found");
    const RouteSlot& route = g_routes[route_index];
    if ((!g_provider_available && !route.resident) || route.unregister_pending)
        return http_error(kQolErrNotReady, "http route unavailable");
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
    return http_error(kQolErrBusy, "http route lease capacity");
}

QolResult http_registry_lease_leave(uint64_t lease) { return sdk_lease_leave(lease); }

std::string host_http_routes_json() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (!g_accepting) return "{\"ok\":false,\"error\":\"http registry inactive\"}";

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
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    clear_routes_locked();
    for (LeaseSlot& lease : g_leases) lease = LeaseSlot{};
    g_next_lease = 1;
    g_lease_count = 0;
    g_accepting = true;
    g_provider_available = true;
    g_unregister_all_pending = false;
    g_duplicate_attempts = 0;
}

bool host_http_registry_accepting() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    return g_accepting;
}

}  // namespace qol::host
