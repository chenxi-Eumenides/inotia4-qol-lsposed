#include "core_settings_config_service.h"

#include <algorithm>
#include <stdexcept>

namespace qol::host {
namespace {

template <typename Callback>
class ScopeExit {
public:
    explicit ScopeExit(Callback callback) noexcept : callback_(std::move(callback)) {}
    ~ScopeExit() noexcept { if (armed_) callback_(); }
    void release() { armed_ = false; }

private:
    Callback callback_;
    bool armed_ = true;
};

thread_local const CoreSettingsConfigService* g_last_runtime_apply_service = nullptr;
thread_local CoreSettingsRuntimeApplyResult g_last_runtime_apply_result{};

ConfigStoreResult not_ready() {
    return {qol::features::core_settings::ConfigStoreError::kNotReady,
            "configuration service not initialized", false};
}

}  // namespace

ConfigStoreResult CoreSettingsConfigService::prepare_features(
    const std::vector<PluginFeatureSeed>& definitions, FeatureDesiredObserver pending, void* context,
    FeatureControlObserver immutable) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = store_.prepare_features(definitions);
    if (result.ok()) {
        feature_model_ = true;
        desired_observer_ = pending;
        desired_context_ = context;
        immutable_observer_ = immutable;
    }
    return result;
}

ConfigStoreSeedResult CoreSettingsConfigService::seed_features() {
    std::shared_ptr<EffectEvent> event;
    ConfigStoreSeedResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == CoreSettingsConfigServiceState::kUninitialized) return {not_ready()};
        result = store_.seed_features();
        if (store_.degraded_read_only()) state_ = CoreSettingsConfigServiceState::kDegradedReadOnly;
        else if (result.result.ok()) state_ = CoreSettingsConfigServiceState::kReady;
        else if (result.memory_committed) state_ = CoreSettingsConfigServiceState::kReadyUnpersisted;
        if (result.result.ok() && result.durable_commit) {
            std::vector<PluginDesiredSnapshot> desired;
            store_.desired_snapshots(&desired);
            event = enqueue_effect_locked(++revision_, {}, {}, nullptr, {},
                                          desired_observer_, desired_context_, std::move(desired));
        }
    }
    if (event) (void)dispatch_effects(event);
    return result;
}

ConfigStoreResult CoreSettingsConfigService::read_feature_namespace(const std::string& id, JsonValue* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.read_feature_namespace(id, out);
}

ConfigStoreResult CoreSettingsConfigService::desired_snapshots(std::vector<PluginDesiredSnapshot>* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.desired_snapshots(out);
}

CoreSettingsConfigCommit CoreSettingsConfigService::commit_feature_model(
    const std::string& id, FeatureConfigField field, const std::string& key, const std::string& json) {
    CoreSettingsConfigCommit result;
    std::shared_ptr<EffectEvent> event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.revision = revision_;
        result.result = store_.update_feature_model(id, field, key, json);
        if (result.result.ok() && result.result.persisted) {
            result.committed = true;
            result.revision = ++revision_;
            state_ = CoreSettingsConfigServiceState::kReady;
            std::vector<PluginDesiredSnapshot> desired;
            store_.desired_snapshots(&desired);
            event = enqueue_effect_locked(result.revision, {}, {}, nullptr, {},
                                          desired_observer_, desired_context_, std::move(desired));
        }
    }
    if (event) {
        const auto outcome = dispatch_effects(event);
        result.apply_state = outcome == EffectDispatchOutcome::kQueued
            ? contracts::SettingsConfigApplyState::kPendingFrame : event->apply_result.state;
        if (outcome == EffectDispatchOutcome::kCompleted) {
            result.apply_error_code = event->apply_result.error_code;
            result.apply_message = event->apply_result.message;
        }
    }
    return result;
}

CoreSettingsConfigCommit CoreSettingsConfigService::commit_feature_model_if_revision(
    const std::string& id, const std::string& key, const std::string& json,
    uint64_t expected_revision) {
    CoreSettingsConfigCommit result;
    std::shared_ptr<EffectEvent> event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.revision = revision_;
        if (state_ == CoreSettingsConfigServiceState::kUninitialized) {
            result.result = not_ready();
            return result;
        }
        if (expected_revision != revision_) {
            result.conflict = true;
            result.result = {qol::features::core_settings::ConfigStoreError::kNone,
                             "configuration revision conflict", false};
            return result;
        }
        result.result = store_.update_feature_model(id, FeatureConfigField::kConfig, key, json);
        if (!result.result.ok() || !result.result.persisted) return result;
        result.committed = true;
        if (state_ == CoreSettingsConfigServiceState::kReadyUnpersisted)
            state_ = CoreSettingsConfigServiceState::kReady;
        result.revision = ++revision_;
        std::vector<PluginDesiredSnapshot> desired;
        (void)store_.desired_snapshots(&desired);
        event = enqueue_effect_locked(result.revision, {}, {}, nullptr, {},
                                      desired_observer_, desired_context_, std::move(desired));
    }
    if (event) {
        const auto outcome = dispatch_effects(event);
        result.apply_state = outcome == EffectDispatchOutcome::kQueued
            ? contracts::SettingsConfigApplyState::kPendingFrame : event->apply_result.state;
        if (outcome == EffectDispatchOutcome::kCompleted) {
            result.apply_error_code = event->apply_result.error_code;
            result.apply_message = event->apply_result.message;
        }
    }
    return result;
}

ConfigStoreResult CoreSettingsConfigService::initialize(const std::string& directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != CoreSettingsConfigServiceState::kUninitialized) {
        return store_.init(directory);
    }
    const ConfigStoreResult result = store_.init(directory);
    if (store_.degraded_read_only()) {
        state_ = CoreSettingsConfigServiceState::kDegradedReadOnly;
    } else if (result.ok()) {
        state_ = CoreSettingsConfigServiceState::kReady;
    } else {
        state_ = CoreSettingsConfigServiceState::kReadyUnpersisted;
    }
    revision_ = 1;
    return result;
}

bool CoreSettingsConfigService::install_runtime_apply(CoreSettingsRuntimeApply apply) {
    if (apply == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (feature_model_) return false;
    if (runtime_apply_ != nullptr) return runtime_apply_ == apply;
    runtime_apply_ = apply;
    return true;
}

std::shared_ptr<CoreSettingsConfigService::EffectEvent>
CoreSettingsConfigService::enqueue_effect_locked(
    uint64_t revision, const ModuleConfig& before, const ModuleConfig& after,
    CoreSettingsRuntimeApply runtime_apply, std::vector<Observer> observers,
    FeatureDesiredObserver desired_observer, void* desired_context,
    std::vector<PluginDesiredSnapshot> desired) {
    auto event = std::make_shared<EffectEvent>();
    event->revision = revision;
    event->before = before;
    event->after = after;
    event->runtime_apply = runtime_apply;
    event->observers = std::move(observers);
    event->desired_observer = desired_observer;
    event->desired_context = desired_context;
    event->desired = std::move(desired);
    if (feature_model_ && immutable_observer_) {
        // The service commit mutex still serializes writers here. Capture every part of
        // this revision before publishing the event; dispatch must never reread live Store.
        event->immutable_observer = immutable_observer_;
        event->snapshot.revision = revision;
        event->snapshot.desired = event->desired;
        for (const auto& plugin : event->desired) {
            JsonValue values;
            const auto read = store_.read_feature_namespace(plugin.plugin_id, &values);
            if (!read.ok()) throw std::runtime_error("committed feature projection unavailable");
            event->snapshot.namespaces.emplace_back(plugin.plugin_id, std::move(values));
        }
    }
    {
        std::lock_guard<std::mutex> effects_lock(effects_mutex_);
        effects_queue_.push_back(event);
    }
    effects_changed_.notify_all();
    return event;
}

CoreSettingsConfigService::EffectDispatchOutcome CoreSettingsConfigService::dispatch_effects(
    const std::shared_ptr<EffectEvent>& awaited) {
    std::unique_lock<std::mutex> lock(effects_mutex_);
    for (;;) {
        if (awaited->completed) return EffectDispatchOutcome::kCompleted;
        if (!effects_dispatching_) break;
        // A callback may reenter the service; its event stays queued for the outer drain.
        if (effects_dispatch_thread_ == std::this_thread::get_id())
            return EffectDispatchOutcome::kQueued;
        effects_changed_.wait(lock, [&] {
            return awaited->completed || !effects_dispatching_;
        });
    }

    std::shared_ptr<EffectEvent> active_event;
    std::string recovery_message = "effect dispatch aborted";
    effects_dispatching_ = true;
    effects_dispatch_thread_ = std::this_thread::get_id();
    ScopeExit restore_dispatcher([&]() noexcept {
        if (!lock.owns_lock()) lock.lock();
        if (active_event != nullptr && !active_event->completed) {
            active_event->apply_result.state = contracts::SettingsConfigApplyState::kRejected;
            active_event->apply_result.error_code = kQolErrInternal;
            active_event->apply_result.message.swap(recovery_message);
            active_event->completed = true;
        }
        effects_dispatching_ = false;
        effects_dispatch_thread_ = std::thread::id{};
        lock.unlock();
        effects_changed_.notify_all();
    });

    while (!effects_queue_.empty()) {
        active_event = effects_queue_.front();
        effects_queue_.pop_front();
        lock.unlock();
        CoreSettingsRuntimeApplyResult apply_result;
        try {
            if (active_event->immutable_observer) {
                try {
                    active_event->immutable_observer(active_event->desired_context, active_event->snapshot);
                    apply_result.state = contracts::SettingsConfigApplyState::kPendingFrame;
                } catch (...) {
                    apply_result.state = contracts::SettingsConfigApplyState::kRejected;
                    apply_result.error_code = kQolErrInternal;
                    apply_result.message = "immutable feature observer callback threw";
                }
            }
            if (active_event->desired_observer != nullptr) {
                try {
                    // This seam only publishes pending desired state. The registry schedules
                    // main-thread reconciliation; no resource work or main-thread waits here.
                    active_event->desired_observer(active_event->desired_context,
                                                    active_event->revision, active_event->desired);
                    apply_result.state = contracts::SettingsConfigApplyState::kPendingFrame;
                } catch (...) {
                    apply_result.state = contracts::SettingsConfigApplyState::kRejected;
                    apply_result.error_code = kQolErrInternal;
                    apply_result.message = "desired observer callback threw";
                }
            }
            if (active_event->runtime_apply != nullptr) {
                try {
                    apply_result = active_event->runtime_apply(active_event->before,
                                                               active_event->after);
                } catch (...) {
                    apply_result.state = contracts::SettingsConfigApplyState::kRejected;
                    apply_result.error_code = kQolErrInternal;
                    apply_result.message = "runtime apply callback threw";
                }
            }
            for (const Observer& observer : active_event->observers) {
                if (observer.callback == nullptr) continue;
                try {
                    observer.callback(observer.context, active_event->revision,
                                      active_event->after);
                } catch (...) {
                    apply_result.state = contracts::SettingsConfigApplyState::kRejected;
                    apply_result.error_code = kQolErrInternal;
                    apply_result.message = "platform observer callback threw";
                }
            }
        } catch (...) {
            apply_result.state = contracts::SettingsConfigApplyState::kRejected;
            apply_result.error_code = kQolErrInternal;
            apply_result.message.swap(recovery_message);
        }
        lock.lock();
        active_event->apply_result = std::move(apply_result);
        active_event->completed = true;
        active_event.reset();
        lock.unlock();
        effects_changed_.notify_all();
        lock.lock();
    }
    effects_dispatching_ = false;
    effects_dispatch_thread_ = std::thread::id{};
    const bool completed = awaited->completed;
    restore_dispatcher.release();
    lock.unlock();
    effects_changed_.notify_all();
    return completed ? EffectDispatchOutcome::kCompleted : EffectDispatchOutcome::kQueued;
}

CoreSettingsRuntimeApplyResult CoreSettingsConfigService::last_runtime_apply_result() const {
    if (g_last_runtime_apply_service == this) return g_last_runtime_apply_result;
    return {qol::contracts::SettingsConfigApplyState::kNotSubmitted,
            kQolErrNotReady, "no config commit receipt on this thread"};
}

ConfigStoreSeedResult CoreSettingsConfigService::seed_registered_plugins(
    const std::vector<std::string>& plugin_ids) {
    std::shared_ptr<EffectEvent> event;
    ModuleConfig before;
    ModuleConfig current;
    uint64_t current_revision = 0;
    ConfigStoreSeedResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == CoreSettingsConfigServiceState::kUninitialized) {
            return {{qol::features::core_settings::ConfigStoreError::kNotReady,
                     "configuration service not initialized", false}, false, false};
        }
        before = store_.snapshot();
        result = store_.seed_registered_plugins(plugin_ids);
        if (store_.degraded_read_only()) {
            state_ = CoreSettingsConfigServiceState::kDegradedReadOnly;
        } else if (!result.result.ok() && result.memory_committed) {
            state_ = CoreSettingsConfigServiceState::kReadyUnpersisted;
        } else if (result.result.ok() && result.result.persisted &&
                   state_ == CoreSettingsConfigServiceState::kReadyUnpersisted) {
            state_ = CoreSettingsConfigServiceState::kReady;
        }
        if (result.result.ok() && result.result.persisted && result.durable_commit) {
            current = store_.snapshot();
            current_revision = ++revision_;
            event = enqueue_effect_locked(current_revision, before, current, runtime_apply_,
                                          observers_);
        }
    }
    if (event != nullptr) (void)dispatch_effects(event);
    return result;
}

ConfigStoreResult CoreSettingsConfigService::snapshot(ModuleConfig* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feature_model_) return {features::core_settings::ConfigStoreError::kValidation,
                               "legacy projection unavailable in feature model", false};
    if (state_ == CoreSettingsConfigServiceState::kUninitialized) return not_ready();
    if (out == nullptr) {
        return {qol::features::core_settings::ConfigStoreError::kInvalidArgument,
                "invalid config snapshot output", false};
    }
    *out = store_.snapshot();
    return {};
}

ConfigStoreResult CoreSettingsConfigService::update(const ModuleConfigPatch& patch) {
    return commit(patch).result;
}

CoreSettingsConfigCommit CoreSettingsConfigService::commit(const ModuleConfigPatch& patch) {
    CoreSettingsConfigCommit commit_result;
    std::shared_ptr<EffectEvent> event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commit_result.revision = revision_;
        if (state_ == CoreSettingsConfigServiceState::kUninitialized) {
            commit_result.result = not_ready();
            return commit_result;
        }
        commit_result.before = store_.snapshot();
        commit_result.after = commit_result.before;
        commit_result.result = store_.update(patch);
        if (commit_result.result.ok() && commit_result.result.persisted) {
            commit_result.after = store_.snapshot();
            commit_result.committed = true;
            if (state_ == CoreSettingsConfigServiceState::kReadyUnpersisted)
                state_ = CoreSettingsConfigServiceState::kReady;
            ++revision_;
            commit_result.revision = revision_;
            event = enqueue_effect_locked(commit_result.revision, commit_result.before,
                                         commit_result.after, runtime_apply_, observers_);
        }
    }
    if (event != nullptr) {
        if (dispatch_effects(event) == EffectDispatchOutcome::kCompleted) {
            commit_result.apply_state = event->apply_result.state;
            commit_result.apply_error_code = event->apply_result.error_code;
            commit_result.apply_message = event->apply_result.message;
        } else {
            commit_result.apply_state = contracts::SettingsConfigApplyState::kPendingFrame;
            commit_result.apply_error_code = 0;
            commit_result.apply_message = "runtime effects queued behind current callback";
        }
    }
    g_last_runtime_apply_service = this;
    g_last_runtime_apply_result = {commit_result.apply_state, commit_result.apply_error_code,
                                   commit_result.apply_message};
    return commit_result;
}

ConfigStoreResult CoreSettingsConfigService::subscribe_platform(
    CoreSettingsPlatformObserver observer, void* context) {
    if (observer == nullptr) {
        return {qol::features::core_settings::ConfigStoreError::kInvalidArgument,
                "null platform observer", false};
    }
    std::shared_ptr<EffectEvent> event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == CoreSettingsConfigServiceState::kUninitialized) return not_ready();
        if (feature_model_) return {features::core_settings::ConfigStoreError::kValidation,
                                   "legacy observer unavailable in feature model", false};
        const auto existing = std::find_if(observers_.begin(), observers_.end(),
            [observer, context](const Observer& item) {
                return item.callback == observer && item.context == context;
            });
        if (existing == observers_.end()) {
            observers_.push_back({observer, context});
            const ModuleConfig current = store_.snapshot();
            event = enqueue_effect_locked(revision_, current, current, nullptr,
                                          {{observer, context}});
        }
    }
    if (event != nullptr) (void)dispatch_effects(event);
    return {};
}

uint64_t CoreSettingsConfigService::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

ConfigStoreResult CoreSettingsConfigService::read_namespace(const std::string& plugin_id,
                                                             JsonValue* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == CoreSettingsConfigServiceState::kUninitialized) return not_ready();
    return store_.read_namespace(plugin_id, out);
}

ConfigStoreResult CoreSettingsConfigService::read_registered_json(char* out_json, uint32_t capacity,
                                                                    uint32_t* out_size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == CoreSettingsConfigServiceState::kUninitialized) return not_ready();
    return store_.read_registered_json(out_json, capacity, out_size);
}

ConfigStoreResult CoreSettingsConfigService::update_namespace_key(
    const std::string& caller_plugin_id, const std::string& target_plugin_id,
    const std::string& key, const std::string& json_value) {
    return update_namespace_key_commit(caller_plugin_id, target_plugin_id, key, json_value).result;
}

CoreSettingsConfigCommit CoreSettingsConfigService::update_namespace_key_commit(
    const std::string& caller_plugin_id, const std::string& target_plugin_id,
    const std::string& key, const std::string& json_value) {
    CoreSettingsConfigCommit commit_result;
    std::shared_ptr<EffectEvent> event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commit_result.revision = revision_;
        if (state_ == CoreSettingsConfigServiceState::kUninitialized) {
            commit_result.result = not_ready();
            return commit_result;
        }
        commit_result.before = store_.snapshot();
        commit_result.after = commit_result.before;
        commit_result.result = store_.update_namespace_key(caller_plugin_id, target_plugin_id, key,
                                                           json_value);
        if (commit_result.result.ok() && commit_result.result.persisted) {
            commit_result.after = store_.snapshot();
            commit_result.committed = true;
            if (state_ == CoreSettingsConfigServiceState::kReadyUnpersisted)
                state_ = CoreSettingsConfigServiceState::kReady;
            commit_result.revision = ++revision_;
            event = enqueue_effect_locked(commit_result.revision, commit_result.before,
                                         commit_result.after, runtime_apply_, observers_);
        }
    }
    if (event != nullptr) {
        if (dispatch_effects(event) == EffectDispatchOutcome::kCompleted) {
            commit_result.apply_state = event->apply_result.state;
            commit_result.apply_error_code = event->apply_result.error_code;
            commit_result.apply_message = event->apply_result.message;
        } else {
            commit_result.apply_state = contracts::SettingsConfigApplyState::kPendingFrame;
            commit_result.apply_error_code = 0;
            commit_result.apply_message = "runtime effects queued behind current callback";
        }
    }
    g_last_runtime_apply_service = this;
    g_last_runtime_apply_result = {commit_result.apply_state, commit_result.apply_error_code,
                                   commit_result.apply_message};
    return commit_result;
}

ConfigStoreResult CoreSettingsConfigService::replace_namespace(const std::string& plugin_id,
                                                                const std::string& json_value) {
    std::shared_ptr<EffectEvent> event;
    ConfigStoreResult result;
    ModuleConfig before;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == CoreSettingsConfigServiceState::kUninitialized) return not_ready();
        before = store_.snapshot();
        result = store_.replace_namespace(plugin_id, json_value);
        if (result.ok() && result.persisted) {
            if (state_ == CoreSettingsConfigServiceState::kReadyUnpersisted)
                state_ = CoreSettingsConfigServiceState::kReady;
            const ModuleConfig current = store_.snapshot();
            const uint64_t current_revision = ++revision_;
            event = enqueue_effect_locked(current_revision, before, current, runtime_apply_,
                                          observers_);
        }
    }
    if (event != nullptr) (void)dispatch_effects(event);
    return result;
}

ConfigStoreResult CoreSettingsConfigService::clear_namespace(const std::string& plugin_id) {
    std::shared_ptr<EffectEvent> event;
    ConfigStoreResult result;
    ModuleConfig before;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == CoreSettingsConfigServiceState::kUninitialized) return not_ready();
        before = store_.snapshot();
        result = store_.clear_namespace(plugin_id);
        if (result.ok() && result.persisted) {
            if (state_ == CoreSettingsConfigServiceState::kReadyUnpersisted)
                state_ = CoreSettingsConfigServiceState::kReady;
            const ModuleConfig current = store_.snapshot();
            const uint64_t current_revision = ++revision_;
            event = enqueue_effect_locked(current_revision, before, current, runtime_apply_,
                                          observers_);
        }
    }
    if (event != nullptr) (void)dispatch_effects(event);
    return result;
}

CoreSettingsConfigServiceState CoreSettingsConfigService::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool CoreSettingsConfigService::degraded_read_only() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == CoreSettingsConfigServiceState::kDegradedReadOnly;
}

CoreSettingsConfigService& core_settings_config_service() {
    static CoreSettingsConfigService service;
    return service;
}

}  // namespace qol::host
