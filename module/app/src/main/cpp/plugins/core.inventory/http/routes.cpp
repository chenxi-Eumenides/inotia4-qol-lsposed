#include "routes.h"

#include <cstring>

#include "plugins/core.inventory/http/query_handler.h"
#include "plugins/core.inventory/http/write_handler.h"

namespace qol::features::core_inventory {
namespace {

constexpr const char* kRouteOwnerId = "core.inventory";

// 读侧 5 条 GET；写命令 ctx 指向 kWriteKinds 中对应条目。
constexpr contracts::HttpRoute kInventoryRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/item/inventory", kRouteOwnerId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/money", kRouteOwnerId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/items", kRouteOwnerId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/info", kRouteOwnerId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/{slot}", kRouteOwnerId, 0, 1},
};

struct InventoryWriteRoute {
    const char* path;
    contracts::QolItemWriteKind kind;
};

constexpr InventoryWriteRoute kWriteRoutes[] = {
    {"/api/item/inventory/move_item", contracts::QolItemWriteKind::kMoveItem},
    {"/api/op/inventory/money", contracts::QolItemWriteKind::kSetMoney},
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

// kWriteKinds 提供端点 ctx 的稳定地址；与 kWriteRoutes 一一对应。
contracts::QolItemWriteKind kWriteKinds[sizeof(kWriteRoutes) / sizeof(kWriteRoutes[0])] = {
    contracts::QolItemWriteKind::kMoveItem,
    contracts::QolItemWriteKind::kSetMoney,
};

contracts::HttpEndpointDescriptor kHttpEndpoints[
    sizeof(kInventoryRoutes) / sizeof(kInventoryRoutes[0]) +
    sizeof(kWriteRoutes) / sizeof(kWriteRoutes[0])];

extern const uint32_t kHttpEndpointCount =
    static_cast<uint32_t>(sizeof(kHttpEndpoints) / sizeof(kHttpEndpoints[0]));

// metadata-only 路由：handler 仍在 Kotlin 传输层，owner 归位 core.inventory。
extern const contracts::HttpRouteMetadataDescriptor kHttpRouteMetadata[] = {
    {contracts::HttpMethod::kPost, "/api/op/inventory/set-slot", kRouteOwnerId, 1, 1},
};

extern const uint32_t kHttpRouteMetadataCount =
    static_cast<uint32_t>(sizeof(kHttpRouteMetadata) / sizeof(kHttpRouteMetadata[0]));

void initialize_http_endpoints() {
    if (g_http_endpoints_initialized) return;
    constexpr size_t kQueryRouteCount = sizeof(kInventoryRoutes) / sizeof(kInventoryRoutes[0]);
    constexpr size_t kWriteRouteCount = sizeof(kWriteRoutes) / sizeof(kWriteRoutes[0]);
    static_assert(sizeof(kWriteKinds) / sizeof(kWriteKinds[0]) == kWriteRouteCount,
                  "write route ctx table must match write routes");
    for (size_t index = 0; index < kQueryRouteCount; ++index) {
        const contracts::HttpRoute& route = kInventoryRoutes[index];
        const contracts::HttpRoute* facts = route_facts(route.method, route.path);
        if (facts == nullptr) continue;
        kHttpEndpoints[index] = contracts::HttpEndpointDescriptor{
            route.method, route.path, route.owner, contracts::HttpHandlerKind::kQuery,
            facts->op_gated, facts->lease_required, &query_handler, nullptr, &kQueries[index]};
    }
    for (size_t index = 0; index < kWriteRouteCount; ++index) {
        const InventoryWriteRoute& route = kWriteRoutes[index];
        const contracts::HttpRoute* facts = route_facts(contracts::HttpMethod::kPost, route.path);
        if (facts == nullptr) continue;
        kHttpEndpoints[kQueryRouteCount + index] = contracts::HttpEndpointDescriptor{
            contracts::HttpMethod::kPost, route.path, kRouteOwnerId,
            contracts::HttpHandlerKind::kCommand, facts->op_gated, facts->lease_required,
            nullptr, &inventory_write_command, &kWriteKinds[index]};
    }
    g_http_endpoints_initialized = true;
}

}  // namespace qol::features::core_inventory
