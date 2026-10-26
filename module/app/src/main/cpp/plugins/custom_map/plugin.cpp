#include "plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "feature.h"
#include "../../contracts/game_ports/custom_map_resource_port.h"
#include "../../contracts/game_ports/ports.h"
#include "../../contracts/panels.h"
#include "../../contracts/static_data.h"
#include "../../contracts/ui.h"
#include "../../contracts/ui_confirmation.h"
#include "ui/panel.h"

namespace qol::features::custom_map {
namespace {

constexpr const char* kPluginId = "custom_map";
constexpr const char* kPluginVersion = "0.1.0";
const char* g_install_error = nullptr;
const contracts::UiConfirmationApi* g_confirmation = nullptr;

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kUiComponentsCapabilityId,
     {contracts::kUiContractMajor, contracts::kUiContractMinor}},
    {contracts::kUiPanelsCapabilityId,
     {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
      static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)}},
    {contracts::kUiConfirmationCapabilityId,
     {static_cast<uint16_t>(contracts::kUiConfirmationVersionMajor),
      static_cast<uint16_t>(contracts::kUiConfirmationVersionMinor)}},
};
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kStaticDataCapabilityId,
     {contracts::kStaticDataVersion.major, contracts::kStaticDataVersion.minor}},
};
const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CUSTOM_MAP_RESOURCE,
     {contracts::kQolPortCustomMapResourceVersion.major,
      contracts::kQolPortCustomMapResourceVersion.minor}},
};

const contracts::PanelCallbacks kPanelCallbacks{
    &custom_map_panel_enter, &custom_map_panel_process, &custom_map_panel_f3,
    &custom_map_panel_f4, &custom_map_panel_event};
const contracts::PanelProviderSpec kPanelProvider{
    "custom_map", kPluginId, &custom_map_provider_open, &custom_map_provider_close,
    &custom_map_provider_active, &custom_map_provider_block_reason, &kPanelCallbacks};

void confirmation_dismissed(void*) {}

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
    if (ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        clear_runtime();
        g_install_error = "install failed: custom map resource port unavailable";
        return 1;
    }
    const auto* resource_port =
        static_cast<const contracts::QolCustomMapResourcePort*>(ctx.ports[0]);
    if (resource_port->abi.size < sizeof(*resource_port) ||
        resource_port->abi.major != contracts::kQolPortCustomMapResourceVersion.major ||
        resource_port->abi.minor < contracts::kQolPortCustomMapResourceVersion.minor ||
        resource_port->decode_lzma_container == nullptr ||
        resource_port->allocate_game_buffer == nullptr) {
        clear_runtime();
        g_install_error = "install failed: custom map resource port invalid";
        return 1;
    }
    const void* components_iface = nullptr;
    const auto* sdk = ctx.sdk;
    if (sdk->capabilities.require == nullptr || sdk->panels.register_provider == nullptr ||
        !contracts::succeeded(sdk->capabilities.require(contracts::kUiComponentsCapabilityId,
            {contracts::kUiContractMajor, contracts::kUiContractMinor}, &components_iface)) ||
        components_iface == nullptr) {
        g_install_error = "install failed: UI components unavailable";
        return 1;
    }
    const auto* components = static_cast<const contracts::QolUiComponents*>(components_iface);
    if (components->size < sizeof(*components) || components->major != 1 ||
        components->minor < contracts::kUiContractMinor ||
        !contracts::succeeded(sdk->panels.register_provider(&kPanelProvider))) {
        g_install_error = "install failed: map panel registration failed";
        return 1;
    }
    const void* confirmation_iface = nullptr;
    if (!contracts::succeeded(sdk->capabilities.require(
            contracts::kUiConfirmationCapabilityId,
            {static_cast<uint16_t>(contracts::kUiConfirmationVersionMajor),
             static_cast<uint16_t>(contracts::kUiConfirmationVersionMinor)},
            &confirmation_iface)) || confirmation_iface == nullptr) {
        if (sdk->panels.unregister_owner != nullptr)
            (void)sdk->panels.unregister_owner(kPluginId);
        g_install_error = "install failed: UI confirmation unavailable";
        return 1;
    }
    g_confirmation = static_cast<const contracts::UiConfirmationApi*>(confirmation_iface);
    custom_map_panel_bind(sdk, components, resource_port);
    initialize(ctx.sdk, resource_port);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    if (ctx.sdk != nullptr && ctx.sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        if (contracts::succeeded(ctx.sdk->capabilities.require(
                contracts::kUiPanelsCapabilityId,
                {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
                 static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)}, &iface)) &&
            iface != nullptr) {
            const auto* panels = static_cast<const contracts::UiPanelsApi*>(iface);
            if (panels->deactivate_owner != nullptr) (void)panels->deactivate_owner(kPluginId);
        }
    }
    custom_map_provider_close();
    custom_map_panel_bind(nullptr, nullptr, nullptr);
    if (ctx.sdk != nullptr && ctx.sdk->panels.unregister_owner != nullptr)
        (void)ctx.sdk->panels.unregister_owner(kPluginId);
    g_confirmation = nullptr;
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

QolResult open_panel_feature(contracts::PluginContext& ctx) {
    if (!enabled()) {
        if (g_confirmation == nullptr || g_confirmation->open_text == nullptr) {
            return contracts::error(kQolErrNotReady, "ui_confirmation unavailable", 26);
        }
        contracts::UiConfirmationSpecText prompt{};
        prompt.size = sizeof(prompt);
        std::snprintf(prompt.message, sizeof(prompt.message),
            "请先启用「自定义地图」，然后再打开地图面板。\n当前不会打开地图。");
        prompt.on_confirm = &confirmation_dismissed;
        prompt.on_cancel = &confirmation_dismissed;
        uint64_t token = 0;
        const QolResult result = g_confirmation->open_text(&prompt, &token);
        if (!contracts::succeeded(result)) {
            if (token != 0 && g_confirmation->close != nullptr)
                (void)g_confirmation->close(token);
            return result;
        }
        return contracts::ok();
    }
    if (ctx.sdk == nullptr || ctx.sdk->capabilities.require == nullptr) {
        return contracts::error(kQolErrNotReady, "ui_panels unavailable", 21);
    }
    const void* iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(contracts::kUiPanelsCapabilityId,
            {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
             static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)}, &iface)) || iface == nullptr) {
        return contracts::error(kQolErrNotReady, "ui_panels unavailable", 21);
    }
    const auto* panels = static_cast<const contracts::UiPanelsApi*>(iface);
    if (panels->open == nullptr || !panels->open("custom_map")) {
        return contracts::error(kQolErrBlocked, "custom map panel could not be opened", 36);
    }
    return contracts::ok();
}

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"custom_map.load", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "自定义地图", "导入加载自定义地图",
      [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
    {"custom_map.open_panel", 1, contracts::FeatureCategory::kTools,
     contracts::FeatureType::kEntry, "查看自定义地图", "查看加载的自定义地图信息",
     nullptr, &open_panel_feature, "打开", 0},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    3, kRequires, // ui_components + ui_panels + ui_confirmation
    1, kOptionalRequires, // optional static map names
    0, nullptr,   // provides
    1, kResources,
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &plugin_on_resource_ready,
     kPorts, 1,    // custom map resource adapter
    nullptr, 0,   // routes
    kFeatures, 2,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::custom_map
