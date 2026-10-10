#pragma once

#include <string>
#include <optional>
#include <mutex>
#include <vector>
#include <utility>
#include "contracts/feature.h"

namespace qol::features::core_settings {

struct ModuleConfig {
    std::string listen_address = "0.0.0.0";
    int listen_port = 8088;
    bool stack_limit_increase = false;
    bool move_merge_enabled = false;
    bool op_enabled = false;
    bool extension_bag_enabled = true;
    bool custom_recipe_enabled = false;
    bool simple_mode_enabled = false;
    bool push_all_enabled = false;
    bool auto_sell_enabled = false;
    bool api_enabled = true;
    bool debug_log_enabled = false;
};

enum class ConfigStoreError { kNone, kInvalidArgument, kNotFound, kValidation, kPersistence, kNotReady };

struct ConfigStoreResult {
    ConfigStoreError error = ConfigStoreError::kNone;
    std::string message;
    bool persisted = true;
    bool ok() const { return error == ConfigStoreError::kNone; }
};

struct ConfigStoreSeedResult {
    ConfigStoreResult result;
    bool changed = false;
    bool memory_committed = false;
    bool durable_commit = false;
};

struct ModuleConfigPatch {
    std::optional<std::string> listen_address;
    std::optional<int> listen_port;
    std::optional<bool> stack_limit_increase;
    std::optional<bool> move_merge_enabled;
    std::optional<bool> op_enabled;
    std::optional<bool> extension_bag_enabled;
    std::optional<bool> custom_recipe_enabled;
    std::optional<bool> simple_mode_enabled;
    std::optional<bool> push_all_enabled;
    std::optional<bool> auto_sell_enabled;
    std::optional<bool> api_enabled;
    std::optional<bool> debug_log_enabled;
};

struct JsonValue {
    enum class Kind { kNull, kBoolean, kNumber, kString, kArray, kObject };
    Kind kind = Kind::kNull;
    bool boolean = false;
    std::string scalar;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

ConfigStoreResult parse_json_document(const std::string& json, JsonValue* out);
std::string serialize_json_value(const JsonValue& value);

struct PluginFeatureSeed {
    std::string plugin_id;
    std::vector<contracts::FeatureDefinition> features;
};
struct PluginDesiredSnapshot {
    std::string plugin_id;
    bool desired_enable = true;
    bool any_feature = false;
    bool want_run() const { return desired_enable && any_feature; }
};
enum class FeatureConfigField { kFeature, kConfig, kDesiredEnable };

class ConfigStore {
public:
    ConfigStore();
    // Selects the new model before initialization; no I/O. Legacy wrappers are not used
    // in this mode and will be removed when the product descriptors switch over.
    ConfigStoreResult prepare_features(const std::vector<PluginFeatureSeed>& definitions);
    ConfigStoreSeedResult seed_features();
    ConfigStoreResult read_feature_namespace(const std::string& plugin_id, JsonValue* out) const;
    ConfigStoreResult desired_snapshots(std::vector<PluginDesiredSnapshot>* out) const;
    ConfigStoreResult update_feature_model(const std::string& plugin_id, FeatureConfigField field,
                                           const std::string& key, const std::string& json);
    ConfigStoreSeedResult seed_registered_plugins(const std::vector<std::string>& plugin_ids);
    ConfigStoreResult init(const std::string& external_files_directory);
    ModuleConfig snapshot() const;
    bool degraded_read_only() const;
    ConfigStoreResult update(const ModuleConfigPatch& partial_update);
    ConfigStoreResult read_namespace(const std::string& plugin_id, JsonValue* out);
    ConfigStoreResult read_registered_json(char* out_json, uint32_t capacity,
                                          uint32_t* out_size) const;
    ConfigStoreResult update_namespace_key(const std::string& caller_plugin_id,
                                           const std::string& target_plugin_id,
                                           const std::string& key,
                                           const std::string& json_value);
    ConfigStoreResult replace_namespace(const std::string& plugin_id, const std::string& json_value);
    ConfigStoreResult clear_namespace(const std::string& plugin_id);
    std::vector<std::string> registered_plugin_ids() const;

private:
    mutable std::mutex mutex_;
    std::string path_;
    ModuleConfig config_;
    bool initialized_ = false;
    bool degraded_read_only_ = false;
    bool init_attempted_ = false;
    std::string init_directory_;
    ConfigStoreResult init_result_;
    std::vector<std::pair<std::string, JsonValue>> namespaces_;
    std::vector<std::pair<std::string, JsonValue>> unknown_root_;
    std::vector<std::string> registered_plugin_ids_;
    bool writeback_pending_ = false;
    bool feature_model_ = false;
    std::vector<PluginFeatureSeed> feature_definitions_;
};

}  // namespace qol::features::core_settings
