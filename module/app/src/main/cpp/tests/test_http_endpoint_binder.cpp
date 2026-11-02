#include "host/http_endpoint_binder.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using qol::contracts::HttpEndpointDescriptor;
using qol::contracts::HttpHandlerKind;
using qol::contracts::HttpMethod;
using qol::contracts::HttpQueryHandlerDescriptor;
using qol::contracts::HttpReq;
using qol::contracts::HttpRes;
using qol::contracts::HttpRoute;
using qol::contracts::HttpRouteMetadataDescriptor;
using qol::contracts::PluginDescriptor;
using qol::host::HttpRouteMatch;

bool g_op_enabled = false;
uint32_t g_handler_calls = 0;

bool fake_op_authorizer() { return g_op_enabled; }

QolResult query_handler(const HttpReq*, HttpRes* response, void*) {
    ++g_handler_calls;
    static constexpr char kBody[] = "{\"query\":true}";
    response->status = 200;
    if (response->body == nullptr || response->body_capacity < sizeof(kBody) - 1)
        return qol::contracts::error(kQolErrBlocked, nullptr, 0);
    std::memcpy(response->body, kBody, sizeof(kBody) - 1);
    response->body_size = sizeof(kBody) - 1;
    return qol::contracts::ok();
}

QolResult command_handler(const HttpReq*, HttpRes* response, void*) {
    ++g_handler_calls;
    static constexpr char kBody[] = "{\"command\":true}";
    response->status = 200;
    if (response->body == nullptr || response->body_capacity < sizeof(kBody) - 1)
        return qol::contracts::error(kQolErrBlocked, nullptr, 0);
    std::memcpy(response->body, kBody, sizeof(kBody) - 1);
    response->body_size = sizeof(kBody) - 1;
    return qol::contracts::ok();
}

bool resident_config_route(const HttpReq& request) {
    constexpr char kPath[] = "/api/config/list";
    return request.method == HttpMethod::kGet && request.path != nullptr &&
           request.path_len == sizeof(kPath) - 1 &&
           std::memcmp(request.path, kPath, sizeof(kPath) - 1) == 0;
}

QolResult resident_config_dispatch(const HttpReq&, HttpRes& response) {
    static constexpr char kBody[] = "{\"resident\":true}";
    response.status = 200;
    if (response.body == nullptr || response.body_capacity < sizeof(kBody) - 1)
        return qol::contracts::error(kQolErrBlocked, nullptr, 0);
    std::memcpy(response.body, kBody, sizeof(kBody) - 1);
    response.body_size = sizeof(kBody) - 1;
    return qol::contracts::ok();
}

PluginDescriptor descriptor(const char* id, const HttpRouteMetadataDescriptor* metadata,
                            uint32_t metadata_count, const HttpEndpointDescriptor* endpoints,
                            uint32_t endpoint_count, const HttpQueryHandlerDescriptor* queries,
                            uint32_t query_count) {
    PluginDescriptor result{};
    result.id = id;
    result.version = "1.0.0";
    result.http_queries = queries;
    result.http_query_count = query_count;
    result.http_endpoints = endpoints;
    result.http_endpoint_count = endpoint_count;
    result.http_route_metadata = metadata;
    result.http_route_metadata_count = metadata_count;
    return result;
}

HttpReq get_request(const char* path) {
    return HttpReq{HttpMethod::kGet, path, static_cast<uint32_t>(std::strlen(path)),
                   nullptr, 0, nullptr, 0, nullptr, 0};
}

HttpReq post_request(const char* path) {
    return HttpReq{HttpMethod::kPost, path, static_cast<uint32_t>(std::strlen(path)),
                   nullptr, 0, nullptr, 0, nullptr, 0};
}

HttpRes make_response(uint8_t* body, uint32_t capacity) {
    return HttpRes{body, capacity, 0, 0};
}

void reset() {
    qol::host::http_dispatch_bind_resident_control(nullptr, nullptr);
    qol::host::http_endpoint_binder_reset_for_test();
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_op_enabled = false;
    g_handler_calls = 0;
}

constexpr HttpRouteMetadataDescriptor kMetadata[] = {
    {HttpMethod::kGet, "/api/test/{id}", "public.query", 0, 1},
    {HttpMethod::kGet, "/api/meta/test", "public.metadata", 0, 1},
    {HttpMethod::kPost, "/api/test/typed", "public.command", 0, 1},
    {HttpMethod::kGet, "/api/op/test", "public.op", 1, 1},
};

constexpr HttpEndpointDescriptor kEndpoints[] = {
    {HttpMethod::kPost, "/api/test/typed", "public.command", HttpHandlerKind::kCommand,
     0, 1, nullptr, &command_handler, nullptr},
};

const HttpQueryHandlerDescriptor kQueries[] = {
    {HttpMethod::kGet, "/api/test/{id}", "public.query", &query_handler, nullptr},
};

PluginDescriptor mixed_owner() {
    return descriptor("runtime.owner", kMetadata, 4, kEndpoints, 1, kQueries, 1);
}

void test_optional_provider_rebind_and_owner_separation() {
    reset();
    PluginDescriptor plugin = mixed_owner();
    assert(qol::host::http_endpoint_binder_validate_descriptor(plugin));
    assert(qol::host::http_endpoint_binder_register_owner(plugin));
    assert(!qol::host::http_endpoint_binder_owner_enabled(plugin));
    assert(qol::host::http_endpoint_binder_owner_http_degraded(plugin.id));

    uint64_t lease = 0;
    const QolResult unavailable = qol::host::http_endpoint_binder_route_enter(
        HttpMethod::kGet, "/api/test/31", 12, &fake_op_authorizer, &lease);
    assert(unavailable.code == kQolErrNotReady && lease == 0);

    qol::host::http_endpoint_binder_set_provider_available(true);
    assert(!qol::host::http_endpoint_binder_owner_http_degraded(plugin.id));
    const auto registry = qol::host::http_registry_api();
    const HttpRoute* route = nullptr;
    assert(registry.find(HttpMethod::kGet, "/api/test/{id}", &route).ok == 0);
    assert(route != nullptr && std::strcmp(route->owner, "public.query") == 0);
    HttpRouteMatch owner_match{};
    assert(qol::host::http_registry_find_match(HttpMethod::kGet, "/api/test/31", 12,
                                               &owner_match).ok == 0);
    assert(owner_match.lifecycle_owner != nullptr &&
           std::strcmp(owner_match.lifecycle_owner, "runtime.owner") == 0);
    assert(std::strcmp(owner_match.route.owner, "public.query") == 0);

    uint8_t storage[128]{};
    HttpRes response = make_response(storage, sizeof(storage));
    HttpReq request = get_request("/api/test/31");
    assert(qol::host::http_dispatch_api().dispatch(&request, &response).ok == 0);
    assert(response.status == 200 && g_handler_calls == 1);

    response = make_response(storage, sizeof(storage));
    request = post_request("/api/test/typed");
    assert(qol::host::http_dispatch_api().dispatch(&request, &response).ok == 0);
    assert(response.status == 200 && g_handler_calls == 2);

    response = make_response(storage, sizeof(storage));
    request = get_request("/api/meta/test");
    assert(qol::host::http_dispatch_api().dispatch(&request, &response).code == kQolErrUnsupported);
    assert(response.status == 501);
    uint64_t metadata_lease = 0;
    assert(qol::host::http_endpoint_binder_route_enter(
               HttpMethod::kGet, "/api/meta/test", 14, &fake_op_authorizer,
               &metadata_lease).ok == 0);
    assert(metadata_lease != 0);
    assert(qol::host::http_endpoint_binder_route_leave(metadata_lease).ok == 0);

    g_op_enabled = false;
    const QolResult direct_op_denied = qol::host::http_endpoint_binder_route_enter(
        HttpMethod::kGet, "/api/op/test", 12, &fake_op_authorizer, &lease);
    assert(direct_op_denied.code == kQolErrBlocked && lease == 0);
    g_op_enabled = true;
    const QolResult entered = qol::host::http_endpoint_binder_route_enter(
        HttpMethod::kGet, "/api/op/test", 12, &fake_op_authorizer, &lease);
    assert(entered.ok == 0 && lease != 0);
    assert(qol::host::http_endpoint_binder_route_leave(lease).ok == 0);

    constexpr uint32_t kLeaseCapacity = 256;
    std::array<uint64_t, kLeaseCapacity> held_leases{};
    for (uint64_t& held_lease : held_leases) {
        assert(registry.lease_enter(HttpMethod::kGet, "/api/op/test", &held_lease).ok == 0);
        assert(held_lease != 0);
    }
    lease = 1;
    const QolResult full_binder_enter = qol::host::http_endpoint_binder_route_enter(
        HttpMethod::kGet, "/api/op/test", 12, &fake_op_authorizer, &lease);
    assert(full_binder_enter.code == kQolErrBusy && lease == 0);
    assert(full_binder_enter.message != nullptr &&
           std::strcmp(full_binder_enter.message, "http route lease capacity") == 0);
    for (uint64_t held_lease : held_leases)
        assert(registry.lease_leave(held_lease).ok == 0);

    lease = 0;
    const QolResult query_enter = qol::host::http_endpoint_binder_route_enter(
        HttpMethod::kGet, "/api/test/31", 12, &fake_op_authorizer, &lease);
    assert(query_enter.ok == 0 && lease != 0);
    qol::host::http_endpoint_binder_owner_stop_begin(plugin.id);
    assert(!qol::host::http_endpoint_binder_owner_stop_poll(plugin.id));
    response = make_response(storage, sizeof(storage));
    request = get_request("/api/test/31");
    assert(qol::host::http_dispatch_api().dispatch(&request, &response).code == kQolErrNotReady);
    assert(response.status == 503);
    assert(qol::host::http_endpoint_binder_route_leave(lease).ok == 0);
    assert(qol::host::http_endpoint_binder_owner_stop_poll(plugin.id));

    assert(qol::host::http_endpoint_binder_owner_enabled(plugin));
    lease = 0;
    assert(qol::host::http_endpoint_binder_route_enter(
               HttpMethod::kGet, "/api/test/31", 12, &fake_op_authorizer, &lease).ok == 0);
    assert(lease != 0);
    qol::host::http_endpoint_binder_set_provider_available(false);
    assert(!qol::host::http_endpoint_binder_provider_drain_poll());
    assert(qol::host::http_endpoint_binder_route_leave(lease).ok == 0);
    assert(qol::host::http_endpoint_binder_provider_drain_poll());
    assert(qol::host::http_endpoint_binder_all_stops_drained());
    assert(qol::host::http_endpoint_binder_route_enter(
               HttpMethod::kGet, "/api/test/31", 12, &fake_op_authorizer, &lease).code ==
           kQolErrNotReady);
    qol::host::http_endpoint_binder_set_provider_available(true);
    assert(qol::host::http_endpoint_binder_route_enter(
               HttpMethod::kGet, "/api/test/31", 12, &fake_op_authorizer, &lease).ok == 0);
    assert(lease != 0);
    assert(qol::host::http_endpoint_binder_route_leave(lease).ok == 0);
    response = make_response(storage, sizeof(storage));
    request = get_request("/api/test/31");
    assert(qol::host::http_dispatch_api().dispatch(&request, &response).ok == 0);
    assert(response.status == 200 && g_handler_calls == 3);
}

void test_collision_rollback_and_validator() {
    reset();
    constexpr HttpRouteMetadataDescriptor first_route[] = {
        {HttpMethod::kGet, "/api/collision", "public.first", 0, 1},
    };
    constexpr HttpRouteMetadataDescriptor second_routes[] = {
        {HttpMethod::kGet, "/api/rollback", "public.rollback", 0, 1},
        {HttpMethod::kGet, "/api/collision", "public.second", 0, 1},
    };
    PluginDescriptor first = descriptor("first.owner", first_route, 1, nullptr, 0, nullptr, 0);
    PluginDescriptor second = descriptor("second.owner", second_routes, 2, nullptr, 0, nullptr, 0);
    assert(qol::host::http_endpoint_binder_register_owner(first));
    assert(qol::host::http_endpoint_binder_register_owner(second));
    qol::host::http_endpoint_binder_set_provider_available(true);
    assert(qol::host::http_endpoint_binder_owner_enabled(first));
    assert(!qol::host::http_endpoint_binder_owner_enabled(second));
    HttpRouteMatch match{};
    assert(qol::host::http_registry_find_match(HttpMethod::kGet, "/api/rollback", 13,
                                               &match).ok == 0);
    assert(match.unregister_pending && match.lifecycle_owner != nullptr &&
           std::strcmp(match.lifecycle_owner, "second.owner") == 0);
    uint8_t storage[64]{};
    HttpRes response = make_response(storage, sizeof(storage));
    HttpReq request = get_request("/api/rollback");
    assert(qol::host::http_dispatch_api().dispatch(&request, &response).code == kQolErrNotReady);
    assert(response.status == 503);

    constexpr HttpRouteMetadataDescriptor bad_route[] = {
        {HttpMethod::kPost, "/api/bad", "public.bad", 2, 1},
    };
    PluginDescriptor invalid = descriptor("invalid.owner", bad_route, 1, nullptr, 0, nullptr, 0);
    assert(!qol::host::http_endpoint_binder_validate_descriptor(invalid));
    assert(!qol::host::http_endpoint_binder_register_owner(invalid));

    PluginDescriptor missing_table = descriptor("missing.table", nullptr, 1,
                                                 nullptr, 0, nullptr, 0);
    assert(!qol::host::http_endpoint_binder_validate_descriptor(missing_table));
    missing_table.http_route_metadata_count = 257;
    missing_table.http_route_metadata = bad_route;
    assert(!qol::host::http_endpoint_binder_validate_descriptor(missing_table));
}

void test_resident_routes_survive_provider_absence() {
    reset();
    qol::host::http_dispatch_bind_resident_control(&resident_config_route,
                                                   &resident_config_dispatch);
    uint64_t lease = 0;
    assert(qol::host::http_endpoint_binder_route_enter(
               HttpMethod::kGet, "/api/config/list", 16, nullptr, &lease).ok == 0);
    assert(lease != 0);
    HttpRouteMatch match{};
    assert(qol::host::http_registry_find_match(HttpMethod::kGet, "/api/config/list", 16,
                                               &match).ok == 0);
    assert(match.resident && match.lifecycle_owner == nullptr);
    assert(qol::host::http_endpoint_binder_route_leave(lease).ok == 0);

    uint8_t body[64]{};
    HttpRes response = make_response(body, sizeof(body));
    const HttpReq request = get_request("/api/config/list");
    assert(qol::host::http_dispatch_api().dispatch(&request, &response).ok == 0);
    assert(response.status == 200);
    assert(std::string(reinterpret_cast<char*>(body), response.body_size) ==
           "{\"resident\":true}");
}

}  // namespace

int main() {
    test_optional_provider_rebind_and_owner_separation();
    test_collision_rollback_and_validator();
    test_resident_routes_survive_provider_absence();
    std::printf("http_endpoint_binder: all checks passed\n");
    return 0;
}
