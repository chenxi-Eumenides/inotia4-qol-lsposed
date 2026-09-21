#include "plugin.h"

#include "contracts/host_sdk.h"

namespace qol::features::device_probe_provider {
namespace {

constexpr const char* kPluginId = "device_probe_provider";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kCapabilityId = "probe.kernel.assembly";
constexpr QolVersion kCapabilityVersion{1, 0};

struct ProbeKernelAssemblyApi {
    uint32_t abi_version;
};

constexpr ProbeKernelAssemblyApi kProbeKernelAssemblyApi{1};
const contracts::CapabilityProvisionRef kProvides[] = {
    {kCapabilityId, kCapabilityVersion, &kProbeKernelAssemblyApi},
};

void probe_log(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

const contracts::QolHostSdk* g_sdk = nullptr;

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    if (g_sdk == nullptr) return 1;
    probe_log(g_sdk, "[device_probe] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    probe_log(ctx.sdk, "[device_probe] provider deactivate");
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    probe_log(ctx.sdk, "[device_probe] provider on_enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    probe_log(ctx.sdk, "[device_probe] provider on_disabled: capability withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_sdk != nullptr;
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
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::device_probe_provider
