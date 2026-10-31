#include "plugin.h"

#include "model/store.h"

#include <atomic>
#include <cstdint>

namespace qol::features::extra_bag {
namespace {

constexpr const char* kPluginId = "extra_bag";
constexpr const char* kPluginVersion = "0.1.0";

std::atomic<bool> g_enabled{false};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr) return 1;
    model::store().clear();
    g_enabled.store(true, std::memory_order_release);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(false, std::memory_order_release);
    model::store().clear();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(true, std::memory_order_release);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(false, std::memory_order_release);
    model::store().clear();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_enabled.load(std::memory_order_acquire);
    out.error = nullptr;
}

QolResult feature_toggle(contracts::PluginContext&, uint8_t) {
    return contracts::ok();
}

const contracts::FeatureDefinition kFeatures[] = {
    {"extra_bag.enabled", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "额外背包（原型）", "", &feature_toggle, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,  // requires
    0, nullptr,  // optional_requires
    0, nullptr,  // provides
    0, nullptr,  // resources
    0, nullptr,  // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,     // on_resource_ready
    nullptr, 0,  // ports
    nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::extra_bag
