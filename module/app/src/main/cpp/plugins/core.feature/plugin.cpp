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
constexpr contracts::CapabilityRequirement kOptionalHttpRequires[] = {
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

const contracts::HttpEndpointDescriptor kHttpEndpoints[] = {
    {contracts::HttpMethod::kPost, kSkillUsagePath, kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &skill_usage_command, nullptr},
};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr) return 1;
    g_sdk = ctx.sdk;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_sdk = nullptr;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_sdk != nullptr;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"core.feature.compatibility_routes", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "迁移占位路由", "",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    2,
    kOptionalHttpRequires,
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
    nullptr, 0, nullptr, 0,
    kFeatures, 1,
    kHttpEndpoints, 1,
    nullptr,
    nullptr, 0,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::core_feature
