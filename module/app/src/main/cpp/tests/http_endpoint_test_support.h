#pragma once

#include "contracts/plugin.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"

#include <cstring>

namespace qol::tests {

inline QolResult register_route_if_missing(const contracts::QolHttpRegistryApi& registry,
                                           const contracts::HttpRoute& route) {
    const contracts::HttpRoute* existing = nullptr;
    const QolResult found = registry.find(route.method, route.path, &existing);
    if (contracts::succeeded(found)) {
        if (existing != nullptr && existing->owner != nullptr && route.owner != nullptr &&
            std::strcmp(existing->owner, route.owner) == 0 &&
            existing->op_gated == route.op_gated &&
            existing->lease_required == route.lease_required) {
            return contracts::ok();
        }
        constexpr char message[] = "route metadata mismatch";
        return contracts::error(kQolErrConflict, message, sizeof(message) - 1);
    }
    return registry.register_route(&route);
}

inline QolResult register_plugin_http(const contracts::PluginDescriptor& descriptor) {
    const contracts::QolHttpRegistryApi registry = host::http_registry_api();
    const contracts::QolHttpDispatchApi dispatch = host::http_dispatch_api();
    for (uint32_t index = 0; index < descriptor.http_endpoint_count; ++index) {
        const contracts::HttpEndpointDescriptor& endpoint = descriptor.http_endpoints[index];
        const contracts::HttpRoute route{endpoint.method, endpoint.path, endpoint.route_owner,
                                         endpoint.op_required, endpoint.lease_required};
        QolResult result = register_route_if_missing(registry, route);
        if (!contracts::succeeded(result)) return result;
        if (endpoint.kind == contracts::HttpHandlerKind::kQuery) {
            result = dispatch.register_query(endpoint.method, endpoint.path, endpoint.route_owner,
                                             endpoint.query_handler, endpoint.ctx);
        } else {
            result = dispatch.register_command(endpoint.method, endpoint.path, endpoint.route_owner,
                                               endpoint.command_handler, endpoint.ctx);
        }
        if (!contracts::succeeded(result)) return result;
    }
    for (uint32_t index = 0; index < descriptor.http_route_metadata_count; ++index) {
        const contracts::HttpRouteMetadataDescriptor& metadata =
            descriptor.http_route_metadata[index];
        const contracts::HttpRoute route{metadata.method, metadata.path, metadata.route_owner,
                                         metadata.op_required, metadata.lease_required};
        const QolResult result = register_route_if_missing(registry, route);
        if (!contracts::succeeded(result)) return result;
    }
    return contracts::ok();
}

}  // namespace qol::tests
