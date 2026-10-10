#pragma once

#include "core_settings_config_store.h"
#include "contracts/settings_config.h"

#include <mutex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

namespace qol::host {

using qol::features::core_settings::ConfigStoreResult;
using qol::features::core_settings::ConfigStoreSeedResult;
using qol::features::core_settings::JsonValue;
using qol::features::core_settings::ModuleConfig;
using qol::features::core_settings::ModuleConfigPatch;
using qol::features::core_settings::PluginFeatureSeed;
using qol::features::core_settings::PluginDesiredSnapshot;
using qol::features::core_settings::FeatureConfigField;
using FeatureDesiredObserver = void (*)(void*, uint64_t,
                                        const std::vector<PluginDesiredSnapshot>&);
struct FeatureControlSnapshot {
    uint64_t revision = 0;
    std::vector<PluginDesiredSnapshot> desired;
    std::vector<std::pair<std::string, JsonValue>> namespaces;
};
using FeatureControlObserver = void (*)(void*, const FeatureControlSnapshot&);

enum class CoreSettingsConfigServiceState {
    kUninitialized,
    kReady,
    kReadyUnpersisted,
    kDegradedReadOnly,
};

struct CoreSettingsConfigCommit {
    ConfigStoreResult result;
    ModuleConfig before;
    ModuleConfig after;
    uint64_t revision = 0;
    bool committed = false;
    contracts::SettingsConfigApplyState apply_state = contracts::SettingsConfigApplyState::kNotSubmitted;
    int32_t apply_error_code = 0;
    std::string apply_message;
};

struct CoreSettingsRuntimeApplyResult {
    contracts::SettingsConfigApplyState state = contracts::SettingsConfigApplyState::kUnchanged;
    int32_t error_code = 0;
    std::string message;
};

using CoreSettingsPlatformObserver = void (*)(void* context, uint64_t revision,
                                               const ModuleConfig& config);
using CoreSettingsRuntimeApply = CoreSettingsRuntimeApplyResult (*)(const ModuleConfig& before,
                                                                    const ModuleConfig& after);

class CoreSettingsConfigService {
public:
    CoreSettingsConfigService() = default;
    ConfigStoreResult prepare_features(const std::vector<PluginFeatureSeed>& definitions,
                                       FeatureDesiredObserver pending, void* context,
                                       FeatureControlObserver immutable = nullptr);
    ConfigStoreSeedResult seed_features();
    ConfigStoreResult read_feature_namespace(const std::string&, JsonValue*) const;
    ConfigStoreResult desired_snapshots(std::vector<PluginDesiredSnapshot>*) const;
    CoreSettingsConfigCommit commit_feature_model(const std::string&, FeatureConfigField,
                                                 const std::string&, const std::string&);
    // Internal transition wrappers for the not-yet-switched product assembly. The new
    // feature model disables their legacy projection/writers; they are not a compatibility API.
    ConfigStoreResult initialize(const std::string& external_files_directory);
    bool install_runtime_apply(CoreSettingsRuntimeApply apply);
    CoreSettingsRuntimeApplyResult last_runtime_apply_result() const;
    ConfigStoreSeedResult seed_registered_plugins(const std::vector<std::string>& plugin_ids);
    ConfigStoreResult snapshot(ModuleConfig* out) const;
    ConfigStoreResult update(const ModuleConfigPatch& patch);
    CoreSettingsConfigCommit commit(const ModuleConfigPatch& patch);
    CoreSettingsConfigCommit update_namespace_key_commit(const std::string& caller_plugin_id,
                                                         const std::string& target_plugin_id,
                                                         const std::string& key,
                                                         const std::string& json_value);
    ConfigStoreResult subscribe_platform(CoreSettingsPlatformObserver observer, void* context);
    bool report_bind_failure(uint64_t revision, int failed_port);
    uint64_t revision() const;
    ConfigStoreResult read_namespace(const std::string& plugin_id, JsonValue* out);
    ConfigStoreResult read_registered_json(char* out_json, uint32_t capacity,
                                           uint32_t* out_size) const;
    ConfigStoreResult update_namespace_key(const std::string& caller_plugin_id,
                                           const std::string& target_plugin_id,
                                           const std::string& key,
                                           const std::string& json_value);
    ConfigStoreResult replace_namespace(const std::string& plugin_id, const std::string& json_value);
    ConfigStoreResult clear_namespace(const std::string& plugin_id);
    CoreSettingsConfigServiceState state() const;
    bool degraded_read_only() const;

private:
    friend CoreSettingsConfigService& core_settings_config_service();
    CoreSettingsConfigService(const CoreSettingsConfigService&) = delete;
    CoreSettingsConfigService& operator=(const CoreSettingsConfigService&) = delete;

    mutable std::mutex mutex_;
    qol::features::core_settings::ConfigStore store_;
    CoreSettingsConfigServiceState state_ = CoreSettingsConfigServiceState::kUninitialized;
    uint64_t revision_ = 0;
    struct Observer {
        CoreSettingsPlatformObserver callback = nullptr;
        void* context = nullptr;
    };
    struct EffectEvent {
        uint64_t revision = 0;
        ModuleConfig before;
        ModuleConfig after;
        CoreSettingsRuntimeApply runtime_apply = nullptr;
        std::vector<Observer> observers;
        CoreSettingsRuntimeApplyResult apply_result;
        bool completed = false;
        FeatureDesiredObserver desired_observer = nullptr;
        void* desired_context = nullptr;
        std::vector<PluginDesiredSnapshot> desired;
        FeatureControlObserver immutable_observer = nullptr;
        FeatureControlSnapshot snapshot;
    };
    enum class EffectDispatchOutcome { kCompleted, kQueued };

    // Events are enqueued while mutex_ still protects their durable revision. This separate
    // queue serializes effects/callbacks without holding the Store state lock during dispatch.
    std::shared_ptr<EffectEvent> enqueue_effect_locked(
        uint64_t revision, const ModuleConfig& before, const ModuleConfig& after,
        CoreSettingsRuntimeApply runtime_apply, std::vector<Observer> observers,
        FeatureDesiredObserver desired_observer = nullptr, void* desired_context = nullptr,
        std::vector<PluginDesiredSnapshot> desired = {});
    EffectDispatchOutcome dispatch_effects(const std::shared_ptr<EffectEvent>& awaited);

    std::vector<Observer> observers_;
    CoreSettingsRuntimeApply runtime_apply_ = nullptr;
    std::mutex effects_mutex_;
    std::condition_variable effects_changed_;
    std::deque<std::shared_ptr<EffectEvent>> effects_queue_;
    bool effects_dispatching_ = false;
    std::thread::id effects_dispatch_thread_;
    FeatureDesiredObserver desired_observer_ = nullptr;
    void* desired_context_ = nullptr;
    bool feature_model_ = false;
    FeatureControlObserver immutable_observer_ = nullptr;
};

CoreSettingsConfigService& core_settings_config_service();

}  // namespace qol::host
