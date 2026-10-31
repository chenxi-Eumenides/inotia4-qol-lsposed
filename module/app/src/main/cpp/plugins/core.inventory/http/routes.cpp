#include "routes.h"

#include <cstring>

#include "plugins/core.inventory/http/query_handler.h"

namespace qol::features::core_inventory {
namespace {

constexpr const char* kRouteOwnerId = "core.inventory";

constexpr contracts::HttpRoute kInventoryRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/item/inventory", kRouteOwnerId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/money", kRouteOwnerId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/items", kRouteOwnerId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/info", kRouteOwnerId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/{slot}", kRouteOwnerId, 0, 1},
};
#define QOL_HTTP_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
const contracts::HttpRoute kRouteFacts[] = {
#include "plugins/core.inventory/routes.inc"
};
#undef QOL_HTTP_ROUTE

const contracts::HttpRoute* route_facts(contracts::HttpMethod method, const char* path) {
    for (const contracts::HttpRoute& route : kRouteFacts) {
        if (route.method == method && std::strcmp(route.path, path) == 0) return &route;
    }
    return nullptr;
}
bool g_http_endpoints_initialized = false;

}  // namespace

contracts::HttpEndpointDescriptor kHttpEndpoints[
    sizeof(kInventoryRoutes) / sizeof(kInventoryRoutes[0])];

extern const uint32_t kHttpEndpointCount =
    static_cast<uint32_t>(sizeof(kHttpEndpoints) / sizeof(kHttpEndpoints[0]));

void initialize_http_endpoints() {
    if (g_http_endpoints_initialized) return;
    constexpr size_t kInventoryRouteCount = sizeof(kInventoryRoutes) / sizeof(kInventoryRoutes[0]);
    for (size_t index = 0; index < kInventoryRouteCount; ++index) {
        const contracts::HttpRoute& route = kInventoryRoutes[index];
        const contracts::HttpRoute* facts = route_facts(route.method, route.path);
        if (facts == nullptr) continue;
        kHttpEndpoints[index] = contracts::HttpEndpointDescriptor{
            route.method, route.path, route.owner, contracts::HttpHandlerKind::kQuery,
            facts->op_gated, facts->lease_required, &query_handler, nullptr, &kQueries[index]};
    }
    g_http_endpoints_initialized = true;
}

}  // namespace qol::features::core_inventory
