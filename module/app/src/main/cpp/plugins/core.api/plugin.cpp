#include "plugin.h"

#include <cstdint>

#include "contracts/http.h"

namespace qol::features::core_api {
namespace {

constexpr const char* kPluginId = "core.api";
constexpr const char* kPluginVersion = "0.1.0";

contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kHttpRegistryCapabilityId, contracts::kHttpRegistryVersion, nullptr},
};

#define QOL_HTTP_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
const contracts::HttpRoute kRoutes[] = {
#include "routes.inc"
};
#undef QOL_HTTP_ROUTE

const contracts::QolHttpRegistryApi* g_registry = nullptr;

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.sdk->http.register_route == nullptr) return 1;
    kProvides[0].iface = &ctx.sdk->http;
    g_registry = &ctx.sdk->http;
    for (const contracts::HttpRoute& route : kRoutes) {
        const QolResult result = g_registry->register_route(&route);
        if (!contracts::succeeded(result)) {
            (void)g_registry->unregister_owner(nullptr);
            g_registry = nullptr;
            return 1;
        }
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    const contracts::QolHttpRegistryApi* registry = g_registry;
    if (registry == nullptr && ctx.sdk != nullptr) registry = &ctx.sdk->http;
    if (registry != nullptr && registry->unregister_owner != nullptr) {
        // nullptr 表示整个 HTTP 出口：先冻结新 lease，宿主在途归零后清空路由。
        (void)registry->unregister_owner(nullptr);
    }
    g_registry = nullptr;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_registry != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    0,
    nullptr,
    1,
    kProvides,
    0,
    nullptr,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    nullptr,
    nullptr,
    &plugin_fill_status,
    nullptr,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_api
