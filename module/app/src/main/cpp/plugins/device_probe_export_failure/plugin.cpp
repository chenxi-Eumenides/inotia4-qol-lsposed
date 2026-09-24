#include "plugin.h"

#include "contracts/host_sdk.h"

namespace qol::features::device_probe_export_failure {
namespace {

constexpr const char* kPluginId = "device_probe_export_failure";
constexpr const char* kPluginVersion = "0.1.0";

void missing_export_handler() {}

int plugin_install(contracts::PluginContext& ctx) {
    constexpr char kUnexpectedInstall[] =
        "[device_probe] missing-export plugin_install unexpectedly called";
    if (ctx.sdk != nullptr && ctx.sdk->log.write != nullptr) {
        (void)ctx.sdk->log.write(2, 1, kPluginId, kUnexpectedInstall,
                                 sizeof(kUnexpectedInstall) - 1);
    }
    return 0;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = false;
    out.error = nullptr;
}

const contracts::ResourceClaim kResources[] = {
    {
        "hook:device_probe:missing_export",
        "elf-export:QOL_Preflight_Missing_Export",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "missing export must fail preflight before plugin install",
        nullptr,
        120,
        reinterpret_cast<void*>(&missing_export_handler),
    },
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    0,
    nullptr,
    0,
    nullptr,
    1,
    kResources,
    0,
    nullptr,
    &plugin_install,
    nullptr,
    nullptr,
    nullptr,
    &plugin_fill_status,
    nullptr,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::device_probe_export_failure
