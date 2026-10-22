#include "plugin.h"

#include <cstdint>
#include <cstring>

#include "feature.h"

namespace qol::features::custom_map {
namespace {

constexpr const char* kPluginId = "custom_map";
constexpr const char* kPluginVersion = "0.1.0";
const char* g_install_error = nullptr;

int32_t res_load_to_pool_wrapper(char* path, void** out_buf, int32_t* out_size) {
    return load_resource(path, out_buf, out_size);
}

const contracts::ResourceClaim kResources[] = {
    {
        "hook:RES_LoadToPool",
        "F_RES_LOAD_TO_POOL_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "地图文件加载唯一入口；仅替换 m<数字>.dat，其余资源透传",
        nullptr,
        120,
        reinterpret_cast<void*>(&res_load_to_pool_wrapper),
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    g_install_error = nullptr;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr) {
        clear_runtime();
        g_install_error = "install failed: host SDK unavailable";
        return 1;
    }
    initialize(ctx.sdk);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    clear_runtime();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_enabled(true);
    g_install_error = nullptr;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_enabled(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = enabled();
    out.error = g_install_error;
}

int32_t plugin_on_resource_ready(contracts::PluginContext& ctx, const char* resource_id,
                                 void* backup) {
    (void)ctx;
    if (resource_id == nullptr ||
        std::strcmp(resource_id, "hook:RES_LoadToPool") != 0 || backup == nullptr) {
        return 1;
    }
    set_original_loader(reinterpret_cast<ResLoadToPoolFn>(backup));
    return 0;
}

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"custom_map.load", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "自定义地图文件加载", "从 qol/custom_map 目录加载同名地图文件。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    1, kResources,
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &plugin_on_resource_ready,
    nullptr, 0,   // ports
    nullptr, 0,   // routes
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::custom_map
