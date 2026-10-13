#include "host/http_endpoint_binder.h"

#include <array>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>
#include <vector>

#include "host/http_dispatch.h"
#include "host/http_registry.h"

namespace qol::host {
namespace {

constexpr uint32_t kMaxOwners = 32;
constexpr uint32_t kMaxRoutesPerOwner = 256;
constexpr uint32_t kNoOwner = kMaxOwners;

enum class OwnerState : uint8_t {
    kUnused,
    kRegistered,
    kWaitingProvider,
    kBinding,
    kBound,
    kDraining,
    kDegraded,
    kInactive,
};

struct OwnerBinding {
    const contracts::PluginDescriptor* descriptor = nullptr;
    OwnerState state = OwnerState::kUnused;
    bool active = false;
    bool provider_drain = false;
    bool http_degraded = false;
};

struct PlannedRoute {
    contracts::HttpRoute route{};
    contracts::HttpQueryHandlerFn query = nullptr;
    contracts::HttpCommandHandlerFn command = nullptr;
    void* ctx = nullptr;
};

OwnerBinding g_owners[kMaxOwners];
std::mutex g_binder_mutex;
bool g_initialized = false;
bool g_provider_available = false;

#define QOL_HTTP_RESIDENT_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
const contracts::HttpRoute kResidentRoutes[] = {
#include "host/http_resident_routes.inc"
};
#undef QOL_HTTP_RESIDENT_ROUTE

bool valid_method(contracts::HttpMethod method) {
    return method == contracts::HttpMethod::kGet || method == contracts::HttpMethod::kPost;
}

bool valid_path_template(const char* path) {
    if (path == nullptr || path[0] != '/') return false;
    const std::string_view value(path);
    if (value.size() > 1 && value.back() == '/') return false;
    size_t cursor = 1;
    while (cursor < value.size()) {
        const size_t start = cursor;
        while (cursor < value.size() && value[cursor] != '/') ++cursor;
        const std::string_view segment = value.substr(start, cursor - start);
        if (segment.empty()) return false;
        if (segment.front() == '{') {
            if (segment.size() < 3 || segment.back() != '}' ||
                segment.substr(1, segment.size() - 2).find_first_of("{}") != std::string_view::npos)
                return false;
        } else if (segment.find_first_of("{}") != std::string_view::npos) {
            return false;
        }
        if (cursor < value.size()) ++cursor;
    }
    return true;
}

bool template_matches_path(const char* route_path, const char* actual_path, uint32_t actual_length) {
    if (route_path == nullptr || actual_path == nullptr || actual_length == 0 ||
        actual_path[0] != '/' || (actual_length > 1 && actual_path[actual_length - 1] == '/'))
        return false;
    const std::string_view route(route_path);
    size_t route_cursor = 1;
    uint32_t actual_cursor = 1;
    while (route_cursor < route.size() || actual_cursor < actual_length) {
        const size_t route_start = route_cursor;
        const uint32_t actual_start = actual_cursor;
        while (route_cursor < route.size() && route[route_cursor] != '/') ++route_cursor;
        while (actual_cursor < actual_length && actual_path[actual_cursor] != '/') ++actual_cursor;
        const size_t route_size = route_cursor - route_start;
        const uint32_t actual_size = actual_cursor - actual_start;
        const std::string_view segment = route.substr(route_start, route_size);
        if (route_cursor < route.size()) ++route_cursor;
        if (actual_cursor < actual_length) ++actual_cursor;
        if (route_size == 0 || actual_size == 0) return false;
        if (segment.size() >= 3 && segment.front() == '{' && segment.back() == '}') continue;
        if (route_size != actual_size ||
            std::memcmp(route.data() + route_start, actual_path + actual_start, route_size) != 0)
            return false;
    }
    return route_cursor == route.size() && actual_cursor == actual_length;
}

bool descriptor_matches_path(const contracts::PluginDescriptor& descriptor,
                             contracts::HttpMethod method, const char* path, uint32_t path_len) {
    for (uint32_t index = 0; index < descriptor.http_route_metadata_count; ++index) {
        const auto& route = descriptor.http_route_metadata[index];
        if (route.method == method && template_matches_path(route.path, path, path_len)) return true;
    }
    for (uint32_t index = 0; index < descriptor.http_endpoint_count; ++index) {
        const auto& endpoint = descriptor.http_endpoints[index];
        if (endpoint.method == method && template_matches_path(endpoint.path, path, path_len))
            return true;
    }
    for (uint32_t index = 0; index < descriptor.http_query_count; ++index) {
        const auto& query = descriptor.http_queries[index];
        if (query.method == method && template_matches_path(query.path, path, path_len)) return true;
    }
    return false;
}

bool same_route(const contracts::HttpRoute& left, const contracts::HttpRoute& right) {
    return left.method == right.method && left.op_gated == right.op_gated &&
           left.lease_required == right.lease_required && left.path != nullptr &&
           right.path != nullptr && left.owner != nullptr && right.owner != nullptr &&
           std::strcmp(left.path, right.path) == 0 && std::strcmp(left.owner, right.owner) == 0;
}

uint32_t owner_index_locked(const char* id) {
    if (id == nullptr) return kNoOwner;
    for (uint32_t index = 0; index < kMaxOwners; ++index) {
        const auto* descriptor = g_owners[index].descriptor;
        if (descriptor != nullptr && std::strcmp(descriptor->id, id) == 0) return index;
    }
    return kNoOwner;
}

bool has_http_declarations(const contracts::PluginDescriptor& descriptor) {
    return descriptor.http_route_metadata_count != 0 || descriptor.http_endpoint_count != 0 ||
           descriptor.http_query_count != 0;
}

bool validate_descriptor(const contracts::PluginDescriptor& descriptor) {
    if (descriptor.id == nullptr || descriptor.id[0] == '\0' ||
        descriptor.http_route_metadata_count > kMaxRoutesPerOwner ||
        descriptor.http_endpoint_count > kMaxRoutesPerOwner ||
        descriptor.http_query_count > kMaxRoutesPerOwner ||
        (descriptor.http_route_metadata_count != 0 && descriptor.http_route_metadata == nullptr) ||
        (descriptor.http_endpoint_count != 0 && descriptor.http_endpoints == nullptr) ||
        (descriptor.http_query_count != 0 && descriptor.http_queries == nullptr)) return false;

    for (uint32_t index = 0; index < descriptor.http_route_metadata_count; ++index) {
        const auto& route = descriptor.http_route_metadata[index];
        if (!valid_method(route.method) || !valid_path_template(route.path) ||
            route.route_owner == nullptr || route.route_owner[0] == '\0' ||
            route.op_required > 1 || route.lease_required > 1) return false;
        for (uint32_t previous = 0; previous < index; ++previous) {
            const auto& other = descriptor.http_route_metadata[previous];
            if (other.method == route.method && std::strcmp(other.path, route.path) == 0)
                return false;
        }
    }

    for (uint32_t index = 0; index < descriptor.http_endpoint_count; ++index) {
        const auto& endpoint = descriptor.http_endpoints[index];
        const bool get = endpoint.method == contracts::HttpMethod::kGet;
        const bool post = endpoint.method == contracts::HttpMethod::kPost;
        if (!valid_method(endpoint.method) || !valid_path_template(endpoint.path) ||
            endpoint.route_owner == nullptr || endpoint.route_owner[0] == '\0' ||
            endpoint.op_required > 1 || endpoint.lease_required > 1 ||
            (get && (endpoint.kind != contracts::HttpHandlerKind::kQuery ||
                     endpoint.query_handler == nullptr || endpoint.command_handler != nullptr)) ||
            (post && (endpoint.kind != contracts::HttpHandlerKind::kCommand ||
                      endpoint.command_handler == nullptr || endpoint.query_handler != nullptr)))
            return false;
        for (uint32_t previous = 0; previous < index; ++previous) {
            const auto& other = descriptor.http_endpoints[previous];
            if (other.method == endpoint.method && std::strcmp(other.path, endpoint.path) == 0)
                return false;
        }
        for (uint32_t previous = 0; previous < descriptor.http_route_metadata_count; ++previous) {
            const auto& metadata = descriptor.http_route_metadata[previous];
            if (metadata.method != endpoint.method || std::strcmp(metadata.path, endpoint.path) != 0)
                continue;
            const contracts::HttpRoute endpoint_route{endpoint.method, endpoint.path,
                endpoint.route_owner, endpoint.op_required, endpoint.lease_required};
            const contracts::HttpRoute metadata_route{metadata.method, metadata.path,
                metadata.route_owner, metadata.op_required, metadata.lease_required};
            if (!same_route(endpoint_route, metadata_route)) return false;
        }
    }

    for (uint32_t index = 0; index < descriptor.http_query_count; ++index) {
        const auto& query = descriptor.http_queries[index];
        if (!valid_method(query.method) || !valid_path_template(query.path) ||
            query.owner == nullptr || query.owner[0] == '\0' || query.handler == nullptr) return false;
        for (uint32_t previous = 0; previous < index; ++previous) {
            const auto& other = descriptor.http_queries[previous];
            if (other.method == query.method && std::strcmp(other.path, query.path) == 0)
                return false;
        }
        for (uint32_t previous = 0; previous < descriptor.http_endpoint_count; ++previous) {
            const auto& endpoint = descriptor.http_endpoints[previous];
            if (endpoint.method == query.method && std::strcmp(endpoint.path, query.path) == 0)
                return false;
        }
        for (uint32_t previous = 0; previous < descriptor.http_route_metadata_count; ++previous) {
            const auto& metadata = descriptor.http_route_metadata[previous];
            if (metadata.method == query.method && std::strcmp(metadata.path, query.path) == 0 &&
                std::strcmp(metadata.route_owner, query.owner) != 0) return false;
        }
    }
    return true;
}

bool add_planned_route(std::vector<PlannedRoute>* plan, const contracts::HttpRoute& route,
                       contracts::HttpQueryHandlerFn query,
                       contracts::HttpCommandHandlerFn command, void* ctx) {
    for (PlannedRoute& existing : *plan) {
        if (existing.route.method != route.method ||
            std::strcmp(existing.route.path, route.path) != 0) continue;
        if (!same_route(existing.route, route)) return false;
        if (query == nullptr && command == nullptr) return true;
        if (existing.query != nullptr || existing.command != nullptr) return false;
        existing.query = query;
        existing.command = command;
        existing.ctx = ctx;
        return true;
    }
    plan->push_back(PlannedRoute{route, query, command, ctx});
    return true;
}

bool build_plan(const contracts::PluginDescriptor& descriptor, std::vector<PlannedRoute>* plan) {
    if (plan == nullptr) return false;
    plan->clear();
    plan->reserve(descriptor.http_route_metadata_count + descriptor.http_endpoint_count +
                  descriptor.http_query_count);

    for (uint32_t index = 0; index < descriptor.http_route_metadata_count; ++index) {
        const auto& metadata = descriptor.http_route_metadata[index];
        if (!add_planned_route(plan,
                contracts::HttpRoute{metadata.method, metadata.path, metadata.route_owner,
                                     metadata.op_required, metadata.lease_required},
                nullptr, nullptr, nullptr)) return false;
    }
    for (uint32_t index = 0; index < descriptor.http_endpoint_count; ++index) {
        const auto& endpoint = descriptor.http_endpoints[index];
        const contracts::HttpRoute route{endpoint.method, endpoint.path, endpoint.route_owner,
                                         endpoint.op_required, endpoint.lease_required};
        if (!add_planned_route(plan, route, endpoint.query_handler, endpoint.command_handler,
                               endpoint.ctx)) return false;
    }
    for (uint32_t index = 0; index < descriptor.http_query_count; ++index) {
        const auto& query = descriptor.http_queries[index];
        contracts::HttpRoute route{};
        bool found = false;
        for (const PlannedRoute& candidate : *plan) {
            if (candidate.route.method == query.method &&
                std::strcmp(candidate.route.path, query.path) == 0) {
                route = candidate.route;
                found = true;
                break;
            }
        }
        if (!found) {
            const contracts::HttpRoute* registered = nullptr;
            const QolResult result = http_registry_api().find(query.method, query.path, &registered);
            if (!contracts::succeeded(result) || registered == nullptr) return false;
            route = *registered;
        }
        if (route.owner == nullptr || std::strcmp(route.owner, query.owner) != 0) return false;
        const bool command = query.method == contracts::HttpMethod::kPost;
        if (!add_planned_route(plan, route,
                command ? nullptr : query.handler,
                command ? query.handler : nullptr, query.ctx)) return false;
    }
    return true;
}

void begin_unbind(const char* owner) {
    http_registry_begin_unregister_lifecycle_owner(owner);
    http_dispatch_begin_unregister_owner(owner);
}

bool poll_unbind(const char* owner) {
    const bool routes = http_registry_poll_unregister_lifecycle_owner(owner);
    const bool handlers = http_dispatch_poll_unregister_owner(owner);
    return routes && handlers;
}

bool bind_owner(uint32_t index) {
    const contracts::PluginDescriptor* descriptor = nullptr;
    bool provider_available = false;
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        if (index >= kMaxOwners || g_owners[index].descriptor == nullptr || !g_owners[index].active)
            return false;
        OwnerBinding& binding = g_owners[index];
        if (binding.state == OwnerState::kBound && g_provider_available) return true;
        if (binding.state == OwnerState::kDraining) return false;
        descriptor = binding.descriptor;
        provider_available = g_provider_available;
        if (!has_http_declarations(*descriptor)) {
            binding.state = OwnerState::kBound;
            binding.http_degraded = false;
            return true;
        }
        if (!provider_available) {
            binding.state = OwnerState::kWaitingProvider;
            binding.http_degraded = true;
            return false;
        }
        binding.state = OwnerState::kBinding;
    }

    bool bound = false;
    try {
        std::vector<PlannedRoute> plan;
        bound = build_plan(*descriptor, &plan);
        if (bound) {
            for (const PlannedRoute& item : plan) {
                const QolResult route_result = http_registry_register_route_for_owner(
                    descriptor->id, item.route, false);
                if (!contracts::succeeded(route_result)) {
                    bound = false;
                    break;
                }
            }
        }
        if (bound) {
            for (const PlannedRoute& item : plan) {
                QolResult result = contracts::ok();
                if (item.query != nullptr) {
                    result = http_dispatch_register_query_for_owner(
                        descriptor->id, item.route.method, item.route.path, item.route.owner,
                        item.query, item.ctx);
                } else if (item.command != nullptr) {
                    result = http_dispatch_register_command_for_owner(
                        descriptor->id, item.route.method, item.route.path, item.route.owner,
                        item.command, item.ctx);
                }
                if (!contracts::succeeded(result)) {
                    bound = false;
                    break;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        bound = false;
    }
    if (!bound) {
        (void)http_dispatch_rollback_uncommitted_owner(descriptor->id);
        (void)http_registry_rollback_uncommitted_owner(descriptor->id);
    } else {
        // Dispatcher admission opens first; the registry route commit opens last.
        http_dispatch_set_owner_ready(descriptor->id);
        http_registry_set_owner_ready(descriptor->id);
    }

    bool stale = false;
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        OwnerBinding& binding = g_owners[index];
        stale = !binding.active || !g_provider_available;
        if (stale) {
            binding.state = binding.active ? OwnerState::kWaitingProvider : OwnerState::kDraining;
            binding.http_degraded = binding.active && has_http_declarations(*descriptor);
        } else {
            binding.state = bound ? OwnerState::kBound : OwnerState::kDegraded;
            binding.http_degraded = !bound;
        }
    }
    if (stale) {
        begin_unbind(descriptor->id);
        return false;
    }
    return bound;
}

void ensure_initialized() {
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        if (g_initialized) return;
        g_initialized = true;
        g_provider_available = false;
    }
    http_registry_set_provider_available(false);
    for (const contracts::HttpRoute& route : kResidentRoutes) {
        (void)http_registry_register_route_for_owner(nullptr, route, true);
    }
}

}  // namespace

bool http_endpoint_binder_validate_descriptor(const contracts::PluginDescriptor& descriptor) {
    return validate_descriptor(descriptor);
}

bool http_endpoint_binder_register_owner(const contracts::PluginDescriptor& descriptor) {
    ensure_initialized();
    if (!validate_descriptor(descriptor)) return false;
    std::lock_guard<std::mutex> lock(g_binder_mutex);
    uint32_t free_index = kNoOwner;
    for (uint32_t index = 0; index < kMaxOwners; ++index) {
        if (g_owners[index].descriptor != nullptr &&
            std::strcmp(g_owners[index].descriptor->id, descriptor.id) == 0) {
            g_owners[index].descriptor = &descriptor;
            return true;
        }
        if (free_index == kNoOwner && g_owners[index].descriptor == nullptr) free_index = index;
    }
    if (free_index == kNoOwner) return false;
    g_owners[free_index] = OwnerBinding{};
    g_owners[free_index].descriptor = &descriptor;
    g_owners[free_index].state = OwnerState::kRegistered;
    return true;
}

bool http_endpoint_binder_owner_enabled(const contracts::PluginDescriptor& descriptor) {
    ensure_initialized();
    if (!http_endpoint_binder_register_owner(descriptor)) return false;
    uint32_t index = kNoOwner;
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        index = owner_index_locked(descriptor.id);
        if (index == kNoOwner) return false;
        OwnerBinding& binding = g_owners[index];
        if (binding.state == OwnerState::kDraining) return false;
        binding.active = true;
    }
    return bind_owner(index);
}

void http_endpoint_binder_owner_stop_begin(const char* owner) {
    ensure_initialized();
    if (owner == nullptr || owner[0] == '\0') return;
    bool has_routes = false;
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        const uint32_t index = owner_index_locked(owner);
        if (index != kNoOwner) {
            OwnerBinding& binding = g_owners[index];
            binding.active = false;
            binding.state = OwnerState::kDraining;
            binding.provider_drain = false;
            has_routes = has_http_declarations(*binding.descriptor);
            if (!has_routes) binding.state = OwnerState::kInactive;
        }
    }
    begin_unbind(owner);
}

bool http_endpoint_binder_owner_stop_poll(const char* owner) {
    if (owner == nullptr || owner[0] == '\0') return false;
    bool drained = poll_unbind(owner);
    if (!drained) return false;
    bool complete = true;
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        const uint32_t index = owner_index_locked(owner);
        if (index == kNoOwner) return true;
        OwnerBinding& binding = g_owners[index];
        if (binding.state == OwnerState::kDraining && !binding.provider_drain) {
            binding.state = OwnerState::kInactive;
            binding.http_degraded = false;
        }
        complete = binding.state != OwnerState::kDraining;
    }
    return complete;
}

void http_endpoint_binder_set_provider_available(bool available) {
    ensure_initialized();
    std::array<const char*, kMaxOwners> drain_owners{};
    std::array<uint32_t, kMaxOwners> bind_owners{};
    uint32_t drain_count = 0;
    uint32_t bind_count = 0;
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        if (g_provider_available == available) return;
        g_provider_available = available;
        if (available) {
            for (uint32_t index = 0; index < kMaxOwners; ++index) {
                OwnerBinding& binding = g_owners[index];
                if (binding.descriptor == nullptr || !binding.active) continue;
                if (binding.state == OwnerState::kWaitingProvider ||
                    binding.state == OwnerState::kDegraded)
                    bind_owners[bind_count++] = index;
            }
        } else {
            for (OwnerBinding& binding : g_owners) {
                if (binding.descriptor == nullptr || !binding.active) continue;
                if (binding.state == OwnerState::kBound || binding.state == OwnerState::kBinding) {
                    binding.state = OwnerState::kDraining;
                    binding.provider_drain = true;
                    binding.http_degraded = has_http_declarations(*binding.descriptor);
                    drain_owners[drain_count++] = binding.descriptor->id;
                } else if (has_http_declarations(*binding.descriptor)) {
                    binding.state = OwnerState::kWaitingProvider;
                    binding.http_degraded = true;
                }
            }
        }
    }
    http_registry_set_provider_available(available);
    if (!available) {
        for (uint32_t index = 0; index < drain_count; ++index)
            begin_unbind(drain_owners[index]);
        return;
    }
    for (uint32_t index = 0; index < bind_count; ++index) (void)bind_owner(bind_owners[index]);
}

bool http_endpoint_binder_provider_drain_poll() {
    std::array<const char*, kMaxOwners> owners{};
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        for (const OwnerBinding& binding : g_owners) {
            if (binding.descriptor != nullptr && binding.provider_drain &&
                binding.state == OwnerState::kDraining)
                owners[count++] = binding.descriptor->id;
        }
    }
    bool all_drained = true;
    for (uint32_t index = 0; index < count; ++index) {
        if (!poll_unbind(owners[index])) {
            all_drained = false;
            continue;
        }
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        const uint32_t owner_index = owner_index_locked(owners[index]);
        if (owner_index != kNoOwner) {
            OwnerBinding& binding = g_owners[owner_index];
            if (binding.state == OwnerState::kDraining && binding.provider_drain) {
                binding.state = OwnerState::kWaitingProvider;
                binding.provider_drain = false;
            }
        }
    }
    return all_drained && http_registry_poll_nonresident_leases() &&
           http_dispatch_poll_all_inflight();
}

bool http_endpoint_binder_all_stops_drained() {
    std::lock_guard<std::mutex> lock(g_binder_mutex);
    for (const OwnerBinding& binding : g_owners) {
        if (binding.state == OwnerState::kDraining) return false;
    }
    return true;
}

bool http_endpoint_binder_owner_http_degraded(const char* owner) {
    if (owner == nullptr || owner[0] == '\0') return false;
    ensure_initialized();
    std::lock_guard<std::mutex> lock(g_binder_mutex);
    const uint32_t index = owner_index_locked(owner);
    return index != kNoOwner && g_owners[index].http_degraded;
}

QolResult http_endpoint_binder_route_enter(contracts::HttpMethod method, const char* path,
                                           uint32_t path_len, HttpEndpointOpAuthorizer authorize,
                                           uint64_t* out_lease) {
    ensure_initialized();
    if (out_lease != nullptr) *out_lease = 0;
    if (out_lease == nullptr || path == nullptr || !valid_method(method))
        return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    HttpRouteMatch match{};
    const QolResult found = http_registry_find_match(method, path, path_len, &match);
    if (!contracts::succeeded(found)) {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        for (const OwnerBinding& binding : g_owners) {
            if (binding.descriptor != nullptr &&
                descriptor_matches_path(*binding.descriptor, method, path, path_len))
                return contracts::error(kQolErrNotReady, nullptr, 0);
        }
        return found;
    }
    if (match.unregister_pending) return contracts::error(kQolErrNotReady, nullptr, 0);
    if (!match.resident) {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        if (!g_provider_available)
            return contracts::error(kQolErrNotReady, nullptr, 0);
        const uint32_t index = owner_index_locked(match.lifecycle_owner);
        if ((match.binder_managed && index == kNoOwner) ||
            (index != kNoOwner && (!g_owners[index].active ||
                                   g_owners[index].state != OwnerState::kBound)))
            return contracts::error(kQolErrNotReady, nullptr, 0);
    }
    if (match.route.op_gated != 0 && (authorize == nullptr || !authorize()))
        return contracts::error(kQolErrBlocked, nullptr, 0);
    if (match.route.lease_required == 0) return contracts::ok();
    return http_registry_lease_enter_match(method, path, path_len, out_lease);
}

QolResult http_endpoint_binder_route_leave(uint64_t lease) {
    if (lease == 0) return contracts::ok();
    return http_registry_lease_leave(lease);
}

void http_endpoint_binder_reset_for_test() {
    {
        std::lock_guard<std::mutex> lock(g_binder_mutex);
        for (OwnerBinding& binding : g_owners) binding = OwnerBinding{};
        g_provider_available = true;
        g_initialized = false;
    }
}

}  // namespace qol::host
