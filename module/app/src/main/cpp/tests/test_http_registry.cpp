#include "host/http_registry.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "contracts/http.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::HttpMethod;
using qol::contracts::HttpRoute;
using qol::host::http_registry_api;
using qol::host::http_registry_reset_for_test;

HttpRoute route(HttpMethod method, const char* path, const char* owner, uint8_t op,
                uint8_t lease) {
    return HttpRoute{method, path, owner, op, lease};
}

void test_registration_and_lookup() {
    http_registry_reset_for_test();
    const auto api = http_registry_api();
    const HttpRoute health = route(HttpMethod::kGet, "/api/health", "core.api", 0, 0);
    const HttpRoute op = route(HttpMethod::kPost, "/api/op/test", "api.test", 1, 1);
    CHECK(api.register_route(&health).ok == 0);
    CHECK(api.register_route(&op).ok == 0);
    CHECK(api.register_route(&health).ok != 0);

    const HttpRoute* found = nullptr;
    CHECK(api.find(HttpMethod::kGet, "/api/health", &found).ok == 0);
    CHECK(found != nullptr && found->op_gated == 0 && found->lease_required == 0);
    CHECK(api.find(HttpMethod::kPost, "/api/health", &found).ok != 0);
    CHECK(api.find(HttpMethod::kGet, "/api/missing", &found).ok != 0);
}

void test_list_and_validation() {
    http_registry_reset_for_test();
    const auto api = http_registry_api();
    const HttpRoute valid = route(HttpMethod::kGet, "/api/one", "owner", 0, 1);
    const HttpRoute invalid = route(HttpMethod::kGet, "api/no-leading-slash", "owner", 0, 1);
    CHECK(api.register_route(&valid).ok == 0);
    CHECK(api.register_route(&invalid).ok != 0);
    HttpRoute routes[2]{};
    uint32_t count = 0;
    CHECK(api.list(routes, 2, &count).ok == 0);
    CHECK(count == 1 && std::strcmp(routes[0].path, "/api/one") == 0);
    CHECK(api.list(nullptr, 0, &count).ok != 0 && count == 0);
    CHECK(api.list(routes, 0, &count).ok != 0);
}

void test_lease_and_deactivation() {
    http_registry_reset_for_test();
    const auto api = http_registry_api();
    const HttpRoute route_a = route(HttpMethod::kGet, "/api/a", "owner", 0, 1);
    CHECK(api.register_route(&route_a).ok == 0);
    uint64_t lease = 0;
    CHECK(api.lease_enter(HttpMethod::kGet, "/api/a", &lease).ok == 0 && lease != 0);
    CHECK(api.unregister_owner(nullptr).ok != 0);
    uint64_t rejected_lease = 0;
    CHECK(api.lease_enter(HttpMethod::kGet, "/api/a", &rejected_lease).ok != 0);
    CHECK(api.lease_leave(lease).ok == 0);
    const HttpRoute* found = nullptr;
    CHECK(api.find(HttpMethod::kGet, "/api/a", &found).ok != 0);
    const std::string inactive_routes = qol::host::host_http_routes_json();
    CHECK(inactive_routes.find("\"ok\":false") != std::string::npos);
    CHECK(inactive_routes.find("http registry inactive") != std::string::npos);
    CHECK(api.lease_leave(lease).ok != 0);

    const HttpRoute route_b = route(HttpMethod::kPost, "/api/b", "owner_b", 0, 1);
    CHECK(api.register_route(&route_b).ok == 0);
    CHECK(api.unregister_owner("owner_b").ok == 0);
    CHECK(api.find(HttpMethod::kPost, "/api/b", &found).ok != 0);
}

void test_lease_capacity_is_busy_and_recovers() {
    constexpr uint32_t kLeaseCapacity = 256;
    http_registry_reset_for_test();
    const auto api = http_registry_api();
    const HttpRoute route_a = route(HttpMethod::kGet, "/api/capacity", "owner", 0, 1);
    CHECK(api.register_route(&route_a).ok == 0);

    std::array<uint64_t, kLeaseCapacity> leases{};
    for (uint64_t& lease : leases) {
        const QolResult result = api.lease_enter(HttpMethod::kGet, "/api/capacity", &lease);
        CHECK(result.ok == 0 && lease != 0);
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        uint64_t rejected_lease = 1;
        const QolResult result = api.lease_enter(HttpMethod::kGet, "/api/capacity",
                                                 &rejected_lease);
        CHECK(result.code == kQolErrBusy);
        CHECK(result.message != nullptr &&
              std::strcmp(result.message, "http route lease capacity") == 0);
        CHECK(rejected_lease == 0);
    }

    CHECK(api.lease_leave(leases[0]).ok == 0);
    uint64_t replacement = 0;
    CHECK(api.lease_enter(HttpMethod::kGet, "/api/capacity", &replacement).ok == 0);
    CHECK(replacement != 0);
    CHECK(api.lease_leave(replacement).ok == 0);
    for (uint64_t lease : leases) {
        if (lease != leases[0]) CHECK(api.lease_leave(lease).ok == 0);
    }
    CHECK(api.unregister_owner("owner").ok == 0);
}

}  // namespace

int main() {
    test_registration_and_lookup();
    test_list_and_validation();
    test_lease_and_deactivation();
    test_lease_capacity_is_busy_and_recovers();
    std::printf("http_registry: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
