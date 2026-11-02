#include "plugins/core.item/runtime/core_item_internal.h"

#include <cstring>

#include "plugins/core.item/shop/buy.h"
#include "plugins/core.item/shop/query.h"

namespace qol::features::core_item {
namespace {

// 库存读侧 5 条 GET 已迁往 core.inventory；本表只剩 shop query 与写命令路由。
// shop GET 的 path 与 shop/routes.inc 的 GET 事实行保持一致。
constexpr contracts::HttpRoute kShopRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/item/shop/items", kPluginId, 0, 1},
};
#define QOL_HTTP_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
constexpr contracts::HttpRoute kRouteFacts[] = {
#include "plugins/core.item/routes.inc"
#include "plugins/core.item/shop/routes.inc"
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
    sizeof(kShopRoutes) / sizeof(kShopRoutes[0]) +
    kItemWriteRouteCount];

extern const uint32_t kHttpEndpointCount =
    static_cast<uint32_t>(sizeof(kHttpEndpoints) / sizeof(kHttpEndpoints[0]));

void initialize_http_endpoints() {
    if (g_http_endpoints_initialized) return;
    constexpr size_t kShopRouteCount = sizeof(kShopRoutes) / sizeof(kShopRoutes[0]);
    for (size_t index = 0; index < kShopRouteCount; ++index) {
        const contracts::HttpRoute& shop_route = kShopRoutes[index];
        const contracts::HttpRoute* shop_facts = route_facts(shop_route.method, shop_route.path);
        if (shop_facts == nullptr) continue;
        kHttpEndpoints[index] = contracts::HttpEndpointDescriptor{
            shop_route.method, shop_route.path, shop_route.owner,
            contracts::HttpHandlerKind::kQuery, shop_facts->op_gated,
            shop_facts->lease_required, &shop::query_handler, nullptr, nullptr};
    }
    for (size_t index = 0; index < kItemWriteRouteCount;
         ++index) {
        ItemWriteRoute& route = kItemWriteRoutes[index];
        const contracts::HttpRoute* facts = route_facts(contracts::HttpMethod::kPost, route.path);
        if (facts == nullptr) continue;
        kHttpEndpoints[kShopRouteCount + index] = contracts::HttpEndpointDescriptor{
            contracts::HttpMethod::kPost, route.path, kPluginId,
            contracts::HttpHandlerKind::kCommand, facts->op_gated, facts->lease_required,
            nullptr, route.handler, &route.kind};
    }
    g_http_endpoints_initialized = true;
}

extern const contracts::HttpRouteMetadataDescriptor kHttpRouteMetadata[] = {
    {contracts::HttpMethod::kPost, "/api/op/inventory/set-equip", kPluginId, 1, 1},
};
extern const uint32_t kHttpRouteMetadataCount =
    static_cast<uint32_t>(sizeof(kHttpRouteMetadata) / sizeof(kHttpRouteMetadata[0]));

}  // namespace qol::features::core_item
