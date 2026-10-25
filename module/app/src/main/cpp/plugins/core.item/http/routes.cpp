#include "plugins/core.item/runtime/core_item_internal.h"

#include <cstring>

namespace qol::features::core_item {
namespace {

constexpr contracts::HttpRoute kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/item/inventory", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/money", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/items", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/info", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/{slot}", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/shop/items", kPluginId, 0, 1},
};
#define QOL_HTTP_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
const contracts::HttpRoute kRouteFacts[] = {
#include "plugins/core.item/routes.inc"
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
    sizeof(kRoutes) / sizeof(kRoutes[0]) +
    kItemWriteRouteCount];

extern const uint32_t kHttpEndpointCount =
    static_cast<uint32_t>(sizeof(kHttpEndpoints) / sizeof(kHttpEndpoints[0]));

void initialize_http_endpoints() {
    if (g_http_endpoints_initialized) return;
    for (size_t index = 0; index < sizeof(kRoutes) / sizeof(kRoutes[0]); ++index) {
        const contracts::HttpRoute& route = kRoutes[index];
        const contracts::HttpRoute* facts = route_facts(route.method, route.path);
        if (facts == nullptr) continue;
        kHttpEndpoints[index] = contracts::HttpEndpointDescriptor{
            route.method, route.path, route.owner, contracts::HttpHandlerKind::kQuery,
            facts->op_gated, facts->lease_required, &query_handler, nullptr, &kQueries[index]};
    }
    constexpr size_t kQueryCount = sizeof(kRoutes) / sizeof(kRoutes[0]);
    for (size_t index = 0; index < kItemWriteRouteCount;
         ++index) {
        ItemWriteRoute& route = kItemWriteRoutes[index];
        const contracts::HttpRoute* facts = route_facts(contracts::HttpMethod::kPost, route.path);
        if (facts == nullptr) continue;
        kHttpEndpoints[kQueryCount + index] = contracts::HttpEndpointDescriptor{
            contracts::HttpMethod::kPost, route.path, kPluginId,
            contracts::HttpHandlerKind::kCommand, facts->op_gated, facts->lease_required,
            nullptr, &item_write_command, &route.kind};
    }
    g_http_endpoints_initialized = true;
}

extern const contracts::HttpRouteMetadataDescriptor kHttpRouteMetadata[] = {
    {contracts::HttpMethod::kPost, "/api/op/inventory/set-slot", kPluginId, 1, 1},
    {contracts::HttpMethod::kPost, "/api/op/inventory/set-equip", kPluginId, 1, 1},
};
extern const uint32_t kHttpRouteMetadataCount =
    static_cast<uint32_t>(sizeof(kHttpRouteMetadata) / sizeof(kHttpRouteMetadata[0]));

}  // namespace qol::features::core_item
