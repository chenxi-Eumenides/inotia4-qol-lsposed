#include "plugin.h"

#include <cstring>

#include "contracts/http.h"

namespace qol::features::core_feature {
namespace {

// This migration-only plugin temporarily owns confirmed unimplemented HTTP endpoints.
// Implemented endpoints must move to their domain owners, after which this plugin is removed.
constexpr const char* kPluginId = "core.feature";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kSkillUsagePath = "/api/character/combat/{role}/set_skill_usage";
constexpr char kNotImplementedBody[] = "{\"ok\":false,\"error\":\"not implemented\"}";
constexpr contracts::CapabilityRequirement kRequires[] = {
    {contracts::kHttpRegistryCapabilityId,
     {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId,
     {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor}},
};

const contracts::QolHostSdk* g_sdk = nullptr;

QolResult skill_usage_command(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < sizeof(kNotImplementedBody) - 1) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, kNotImplementedBody, sizeof(kNotImplementedBody) - 1);
    response->body_size = sizeof(kNotImplementedBody) - 1;
    response->status = 501;
    return contracts::ok();
}

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.sdk->http_dispatch.register_command == nullptr) return 1;
    const QolResult result = ctx.sdk->http_dispatch.register_command(
        contracts::HttpMethod::kPost, kSkillUsagePath, kPluginId, &skill_usage_command, nullptr);
    if (!contracts::succeeded(result)) return 1;
    g_sdk = ctx.sdk;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    const contracts::QolHostSdk* sdk = ctx.sdk != nullptr ? ctx.sdk : g_sdk;
    if (sdk != nullptr && sdk->http_dispatch.unregister_owner != nullptr) {
        (void)sdk->http_dispatch.unregister_owner(kPluginId);
    }
    g_sdk = nullptr;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_sdk != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2,
    kRequires,
    0,
    nullptr,
    0,
    nullptr,
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

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::core_feature
