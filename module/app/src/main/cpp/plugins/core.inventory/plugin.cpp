#include "plugin.h"

#include <cstdarg>
#include <cstdio>
#include <cstdint>

namespace qol::features::core_inventory {
namespace {

constexpr const char* kPluginId = "core.inventory";
constexpr const char* kPluginVersion = "0.1.0";
constexpr int32_t kLogDomain = 5;  // QolDomain::kInventory

const contracts::QolHostSdk* g_sdk = nullptr;
bool g_installed = false;
bool g_enabled = false;

void log_line(int32_t level, const char* format, ...) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || format == nullptr) return;
    char message[512]{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written <= 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= sizeof(message)
                                ? static_cast<uint32_t>(sizeof(message) - 1)
                                : static_cast<uint32_t>(written);
    (void)g_sdk->log.write(level, kLogDomain, kPluginId, message, length);
}

int plugin_install(contracts::PluginContext& context) {
    if (context.sdk == nullptr) return 1;
    g_sdk = context.sdk;
    g_installed = true;
    log_line(1, "plugin=%s: installed version=%s mode=skeleton", kPluginId, kPluginVersion);
    return 0;
}

void plugin_deactivate(contracts::PluginContext&) {
    log_line(1, "plugin=%s: deactivated", kPluginId);
    g_sdk = nullptr;
    g_installed = false;
    g_enabled = false;
}

void plugin_on_enabled(contracts::PluginContext&) {
    g_enabled = true;
    log_line(1, "plugin=%s: enabled", kPluginId);
}

void plugin_on_disabled(contracts::PluginContext&) {
    g_enabled = false;
    log_line(1, "plugin=%s: disabled", kPluginId);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_installed && g_enabled;
    out.error = g_installed ? nullptr : "not installed";
}

QolResult feature_toggle(contracts::PluginContext&, uint8_t enabled) {
    log_line(1, "plugin=%s: feature toggle enabled=%u", kPluginId,
             static_cast<unsigned>(enabled));
    return contracts::ok();
}

const contracts::FeatureDefinition kFeatures[] = {
    {"core.inventory.enabled", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "库存层（骨架）", "存储层占位，暂不提供运行时能力。",
     &feature_toggle, nullptr},
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
    nullptr,
    0,
    nullptr,
    0,
    kFeatures,
    1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::core_inventory
