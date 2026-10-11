#include "production.h"
#include "platform.h"
#include "infra/status.h"

#include <atomic>
#include <algorithm>
#include <cstring>

namespace qol::host {
namespace {

struct ProductionFeatures {
    FeatureDirectory directory;
    FeatureController controller{core_settings_config_service()};
    FeatureReconciler reconciler{controller};
    std::vector<std::vector<contracts::QolFeatureCatalogItem>> catalog_items;
    std::vector<contracts::QolFeatureCatalogPlugin> catalog;
    bool sealed = false;
    std::atomic<bool> catalog_ready{false};
    ProductionFeatures() {
        controller.bind_config_validator(&validate_platform_config);
        controller.add_state_observer(&feature_platform_publish, nullptr);
    }
};

ProductionFeatures& production() {
    static ProductionFeatures instance;
    return instance;
}

QolResult error(int32_t code) { return contracts::error(code, nullptr, 0); }

QolResult read_catalog(void* context, const contracts::QolFeatureCatalogPlugin** out,
                       uint32_t* count) {
    if (!out || !count) return error(kQolErrInvalidArgument);
    *out = nullptr;
    *count = 0;
    auto& self = *static_cast<ProductionFeatures*>(context);
    if (!self.catalog_ready.load(std::memory_order_acquire)) return error(kQolErrNotReady);
    *out = self.catalog.data();
    *count = static_cast<uint32_t>(self.catalog.size());
    return contracts::ok();
}

QolResult read_desired(void* context, const char* id, uint8_t* enable, uint8_t* any) {
    if (!id || !enable || !any) return error(kQolErrInvalidArgument);
    *enable = 0;
    *any = 0;
    std::vector<PluginDesiredSnapshot> snapshots;
    auto result = static_cast<ProductionFeatures*>(context)->controller.desired(&snapshots);
    if (!result.ok()) return error(kQolErrNotReady);
    for (const auto& snapshot : snapshots) {
        if (snapshot.plugin_id != id) continue;
        *enable = snapshot.desired_enable;
        *any = snapshot.any_feature;
        return contracts::ok();
    }
    return error(kQolErrNotFound);
}

QolResult read_feature(void* context, const char* id, const char* key, uint8_t* out) {
    return static_cast<ProductionFeatures*>(context)->controller.read_feature(id, key, out);
}
QolResult set_feature(void* context, const char* id, const char* key, uint8_t enabled,
                      contracts::FeatureReceipt* out) {
    return static_cast<ProductionFeatures*>(context)->controller.request_set_feature(id, key, enabled, out);
}
QolResult set_desired(void* context, const char* id, uint8_t enabled,
                      contracts::FeatureReceipt* out) {
    return static_cast<ProductionFeatures*>(context)->controller.request_desired(id, enabled, out);
}
QolResult receipt(void* context, uint64_t id, contracts::FeatureReceipt* out) {
    return static_cast<ProductionFeatures*>(context)->controller.receipt(id, out);
}
QolResult entry(void* context, const char* id, const char* key) {
    return static_cast<ProductionFeatures*>(context)->controller.invoke_entry(id, key);
}
QolResult read_config(void* c, const char* p, const char* key, char* out, uint32_t capacity, uint32_t* size) {
    return static_cast<ProductionFeatures*>(c)->controller.read_config(p, key, out, capacity, size);
}
QolResult set_config(void* c, const char* p, const char* key, const char* json, uint32_t size, contracts::FeatureReceipt* out) {
    return static_cast<ProductionFeatures*>(c)->controller.request_set_config(p, key, json, size, out);
}
QolResult read_actual(void* c, const char* p, int32_t* state, uint8_t* enabled) {
    if (!p || !state || !enabled) return error(kQolErrInvalidArgument);
    if (static_cast<ProductionFeatures*>(c)->controller.host_owned(p)) {
        *state = static_cast<int32_t>(infra::FeatureState::kEnabled);
        *enabled = 1;
        return contracts::ok();
    }
    infra::FeatureStatus status{};
    if (!infra::status_get(p, &status)) return error(kQolErrNotReady);
    *state = static_cast<int32_t>(status.state);
    *enabled = status.enabled;
    return contracts::ok();
}

}  // namespace

const FeatureDirectory& production_feature_directory() { return production().directory; }
FeatureController& production_feature_controller() { return production().controller; }
FeatureReconciler& production_feature_reconciler() { return production().reconciler; }
const contracts::QolFeatureApi& production_feature_api() { return production().controller.api(); }
const contracts::QolFeatureControlApi& production_feature_control_api() {
    static const contracts::QolFeatureControlApi api{
        sizeof(contracts::QolFeatureControlApi), &production(), &read_catalog, &read_desired,
        &read_feature, &set_feature, &set_desired, &receipt, &entry,
        &read_config, &set_config, &read_actual};
    return api;
}

ConfigStoreResult production_feature_register(const contracts::PluginDescriptor& descriptor) {
    auto& self = production();
    if (self.sealed) {
        return {features::core_settings::ConfigStoreError::kValidation,
                "feature registration is sealed", false};
    }
    return self.directory.add(descriptor);
}
ConfigStoreResult production_feature_register_host_owner(const contracts::PluginDescriptor& descriptor) {
    auto& self = production();
    if (self.sealed) return {features::core_settings::ConfigStoreError::kValidation, "feature registration is sealed", false};
    return self.directory.add_host_owner(descriptor);
}

ConfigStoreResult production_feature_bootstrap(const std::string& directory) {
    auto& self = production();
    if (self.sealed) {
        return {features::core_settings::ConfigStoreError::kValidation,
                "feature bootstrap already attempted", false};
    }
    self.sealed = true;
    // Registration is now immutable; these views remain valid across owner replacement.
    self.catalog_items.reserve(self.directory.definitions().size());
    self.catalog.reserve(self.directory.definitions().size());
    for (const auto& plugin : self.directory.definitions()) {
        const bool core_plugin = plugin.plugin_id.compare(0, 5, "core.") == 0;
        const bool core_custom_ui = plugin.plugin_id == "core.custom_ui";
        // Core residency remains outside the player catalog except for the debug-only entry.
        // The settings page belongs to core.settings but its registered id is "settings".
        if ((core_plugin && !core_custom_ui) || plugin.plugin_id == "settings") continue;
        const auto include_feature = [core_custom_ui](const contracts::FeatureDefinition& feature) {
            if (!core_custom_ui) return true;
            return feature.id != nullptr &&
                std::strcmp(feature.id, "core.custom_ui.open_debug_components") == 0 &&
                feature.type == contracts::FeatureType::kEntry &&
                feature.category == contracts::FeatureCategory::kHidden &&
                feature.on_entry != nullptr;
        };
        const auto visible_count = static_cast<size_t>(std::count_if(
            plugin.features.begin(), plugin.features.end(), include_feature));
        if (visible_count == 0) continue;

        self.catalog_items.emplace_back();
        auto& items = self.catalog_items.back();
        items.reserve(visible_count);
        for (const auto& feature : plugin.features) {
            if (!include_feature(feature)) continue;
            items.push_back({feature.id, feature.default_enabled, feature.category,
                             feature.type, feature.name, feature.description,
                             feature.action_label});
        }
        self.catalog.push_back({plugin.plugin_id.c_str(), items.data(),
                                static_cast<uint32_t>(items.size()),
                                static_cast<uint8_t>(std::find(self.directory.host_owners().begin(),
                                    self.directory.host_owners().end(), plugin.plugin_id) != self.directory.host_owners().end())});
    }
    auto result = self.controller.initialize(self.directory, directory, nullptr, nullptr);
    self.catalog_ready.store(true, std::memory_order_release);
    return result;
}

bool production_feature_bind_frame(const HostDeps& deps) {
    auto& self = production();
    if (!self.catalog_ready.load(std::memory_order_acquire)) return false;
    return self.reconciler.bind(deps);
}
uint32_t production_feature_frame_step() { return production().reconciler.step(); }

bool production_feature_bind_management_sdk(contracts::QolHostSdk& sdk) {
    if (contracts::feature_if_available(&sdk) == nullptr ||
        sdk.abi.size < offsetof(contracts::QolHostSdk, feature_control) +
                       sizeof(sdk.feature_control)) return false;
    sdk.feature_control = &production_feature_control_api();
    return true;
}

}  // namespace qol::host
