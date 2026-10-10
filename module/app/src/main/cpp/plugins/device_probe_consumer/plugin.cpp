#include "plugin.h"

#include "contracts/host_sdk.h"

namespace qol::features::device_probe_consumer {
namespace {

constexpr const char* kPluginId = "device_probe_consumer";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kCapabilityId = "probe.kernel.assembly";
constexpr QolVersionRange kRequiresRange{1, 0};

const contracts::CapabilityRequirement kRequires[] = {
    {kCapabilityId, kRequiresRange},
};

void probe_log(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

const contracts::QolHostSdk* g_sdk = nullptr;
const void* g_provider_iface = nullptr;

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return 1;
    const QolResult result = g_sdk->capabilities.require(
        kCapabilityId, kRequiresRange, &g_provider_iface);
    if (!contracts::succeeded(result) || g_provider_iface == nullptr) {
        g_provider_iface = nullptr;
        return 1;
    }
    probe_log(g_sdk, "[device_probe] consumer install: capability injected");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    probe_log(ctx.sdk, "[device_probe] consumer deactivate");
    g_provider_iface = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    probe_log(ctx.sdk, "[device_probe] consumer on_enabled: dependency resolved");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    probe_log(ctx.sdk, "[device_probe] consumer on_disabled: dependency released");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_sdk != nullptr && g_provider_iface != nullptr;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"device_probe_consumer.dependency_probe", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "能力依赖探针", "验证设备探针能力消费与依赖生命周期接线。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    1,
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
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
    nullptr, 0, nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::device_probe_consumer
