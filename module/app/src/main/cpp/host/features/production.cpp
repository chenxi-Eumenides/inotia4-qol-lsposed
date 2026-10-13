#include "production.h"
#include "platform.h"
#include "host/owner_identity.h"
#include "infra/status.h"
#include "infra/log/qol_log.h"

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>

namespace qol::host {
namespace {

constexpr contracts::FeatureCategoryDefinition kFeatureCategories[] = {
    {contracts::FeatureCategory::kGeneral, "基础设置", -9},
    {contracts::FeatureCategory::kBasicOptimization, "基础优化设置", -8},
    {contracts::FeatureCategory::kGameplay, "更多优化功能", -5},
    {contracts::FeatureCategory::kTools, "更多工具", -3},
    {contracts::FeatureCategory::kHidden, "调试开关", -10},
};

struct RuntimeState {
    std::string plugin_id;
    std::string config_json = "{}";
    uint64_t event_revision = 0;
    uint64_t config_revision = 0;
    bool actual_enabled = false;
    bool desired_enabled = false;
};

struct RuntimeSubscription {
    uint64_t id = 0;
    std::string lifecycle_owner_id;
    std::string plugin_id;
    contracts::QolPluginRuntimeObserver observer = nullptr;
    void* context = nullptr;
    uint64_t last_delivered_revision = 0;
    uint32_t in_flight = 0;
    bool retiring = false;
    bool cancelled = false;
};

struct RuntimeDelivery {
    std::shared_ptr<RuntimeSubscription> subscription;
    std::shared_ptr<const RuntimeState> state;
    bool complete = false;
};

class RuntimeEffectsHub {
public:
    QolResult subscribe(const char* plugin_id, const char* lifecycle_owner_id,
                        contracts::QolPluginRuntimeObserver observer, void* context,
                        uint64_t* out_id) {
        // Topic identity selects snapshots; only SDK-self subscriptions have a plugin lifecycle owner.
        if (out_id == nullptr) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
        *out_id = 0;
        if (plugin_id == nullptr || plugin_id[0] == '\0' ||
            (lifecycle_owner_id != nullptr && lifecycle_owner_id[0] == '\0') ||
            observer == nullptr)
            return contracts::error(kQolErrInvalidArgument, nullptr, 0);
        std::shared_ptr<RuntimeDelivery> replay;
        bool become_dispatcher = false;
        const bool reentrant = active_hub_ == this;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto current = states_.find(plugin_id);
            if (current == states_.end()) return contracts::error(kQolErrNotReady, nullptr, 0);
            auto subscription = std::make_shared<RuntimeSubscription>();
            subscription->id = next_subscription_id_++;
            if (lifecycle_owner_id != nullptr)
                subscription->lifecycle_owner_id = lifecycle_owner_id;
            subscription->plugin_id = plugin_id;
            subscription->observer = observer;
            subscription->context = context;
            subscriptions_.emplace(subscription->id, subscription);
            *out_id = subscription->id;
            replay = std::make_shared<RuntimeDelivery>();
            replay->subscription = subscription;
            replay->state = current->second;
            ++subscription->in_flight;
            if (!reentrant) deliveries_.push_back(replay);
            if (!reentrant && !dispatching_) {
                dispatching_ = true;
                become_dispatcher = true;
            }
        }
        if (become_dispatcher) drain();
        else if (reentrant) deliver(replay);
        else {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return replay->complete; });
        }
        return contracts::ok();
    }

    QolResult unsubscribe(uint64_t id, const char* lifecycle_owner_id, bool trusted) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto found = subscriptions_.find(id);
        if (found == subscriptions_.end()) return contracts::error(kQolErrNotFound, nullptr, 0);
        const auto subscription = found->second;
        if (!trusted && (lifecycle_owner_id == nullptr ||
                         subscription->lifecycle_owner_id != lifecycle_owner_id))
            return contracts::error(kQolErrBlocked, nullptr, 0);
        subscription->cancelled = true;
        subscriptions_.erase(found);
        if (active_hub_ == this) return contracts::ok();
        changed_.wait(lock, [&] { return subscription->in_flight == 0; });
        return contracts::ok();
    }

    void unsubscribe_owner(const char* lifecycle_owner_id) {
        if (lifecycle_owner_id == nullptr) return;
        std::vector<std::shared_ptr<RuntimeSubscription>> removed;
        std::unique_lock<std::mutex> lock(mutex_);
        for (const auto& entry : subscriptions_) {
            const auto& subscription = entry.second;
            if (subscription->lifecycle_owner_id != lifecycle_owner_id) continue;
            subscription->retiring = true;
            removed.push_back(subscription);
        }
        if (active_hub_ == this) {
            for (const auto& subscription : removed) {
                subscription->cancelled = true;
                subscriptions_.erase(subscription->id);
            }
            return;
        }
        changed_.wait(lock, [&] {
            return std::all_of(removed.begin(), removed.end(),
                [](const auto& subscription) { return subscription->in_flight == 0; });
        });
        for (const auto& subscription : removed) {
            subscription->cancelled = true;
            subscriptions_.erase(subscription->id);
        }
    }

    void publish_config(const FeatureControlSnapshot& snapshot) {
        struct PendingState { RuntimeState state; bool actual_known; };
        std::vector<PendingState> pending;
        pending.reserve(snapshot.namespaces.size());
        for (const auto& item : snapshot.namespaces) {
            const JsonValue* config = nullptr;
            for (const auto& section : item.second.object) {
                if (section.first == "config") { config = &section.second; break; }
            }
            RuntimeState state;
            state.plugin_id = item.first;
            state.config_json = config != nullptr
                ? features::core_settings::serialize_json_value(*config) : "{}";
            state.config_revision = snapshot.revision;
            const auto desired = std::find_if(snapshot.desired.begin(), snapshot.desired.end(),
                [&item](const PluginDesiredSnapshot& candidate) {
                    return candidate.plugin_id == item.first;
                });
            state.desired_enabled = desired != snapshot.desired.end() && desired->want_run();
            infra::FeatureStatus status{};
            const bool actual_known = infra::status_get(item.first.c_str(), &status);
            // Bootstrap has no actual owners yet; subsequent changes use registry transition
            // publications so a worker-side config snapshot never reads mutable registry state.
            state.actual_enabled = actual_known && status.state == infra::FeatureState::kEnabled &&
                status.enabled;
            pending.push_back({std::move(state), actual_known});
        }
        bool become_dispatcher = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& update : pending) {
                const auto found = states_.find(update.state.plugin_id);
                if (found != states_.end() && found->second->config_revision >= update.state.config_revision)
                    continue;
                if (found != states_.end()) {
                    update.state.actual_enabled = found->second->actual_enabled;
                } else if (!update.actual_known) {
                    update.state.actual_enabled = false;
                }
                update.state.event_revision = ++event_revision_;
                publish_locked(std::make_shared<const RuntimeState>(std::move(update.state)));
            }
            if (!deliveries_.empty() && !dispatching_) {
                dispatching_ = true;
                become_dispatcher = true;
            }
        }
        if (become_dispatcher) drain();
    }

    void publish_actual(const char* plugin_id, bool enabled) {
        if (plugin_id == nullptr || plugin_id[0] == '\0') return;
        bool become_dispatcher = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = states_.find(plugin_id);
            if (found == states_.end() || found->second->actual_enabled == enabled) return;
            RuntimeState state = *found->second;
            state.actual_enabled = enabled;
            state.event_revision = ++event_revision_;
            publish_locked(std::make_shared<const RuntimeState>(std::move(state)));
            if (!deliveries_.empty() && !dispatching_) {
                dispatching_ = true;
                become_dispatcher = true;
            }
        }
        if (become_dispatcher) drain();
    }

private:
    void publish_locked(std::shared_ptr<const RuntimeState> state) {
        states_[state->plugin_id] = state;
        for (const auto& entry : subscriptions_) {
            const auto& subscription = entry.second;
            if (!subscription->retiring && !subscription->cancelled &&
                subscription->plugin_id == state->plugin_id) {
                auto delivery = std::make_shared<RuntimeDelivery>();
                delivery->subscription = subscription;
                delivery->state = state;
                ++subscription->in_flight;
                deliveries_.push_back(std::move(delivery));
            }
        }
    }

    void drain() {
        RuntimeEffectsHub* previous_hub = active_hub_;
        active_hub_ = this;
        for (;;) {
            std::shared_ptr<RuntimeDelivery> delivery;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (deliveries_.empty()) {
                    dispatching_ = false;
                    changed_.notify_all();
                    active_hub_ = previous_hub;
                    return;
                }
                delivery = std::move(deliveries_.front());
                deliveries_.pop_front();
            }
            deliver(delivery);
        }
    }

    void deliver(const std::shared_ptr<RuntimeDelivery>& delivery) {
        const auto& subscription = delivery->subscription;
        bool invoke = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!subscription->cancelled &&
                delivery->state->event_revision > subscription->last_delivered_revision) {
                subscription->last_delivered_revision = delivery->state->event_revision;
                invoke = true;
            }
        }
        if (invoke) {
            contracts::QolPluginRuntimeEffect effect{};
            effect.struct_size = sizeof(effect);
            effect.event_revision = delivery->state->event_revision;
            effect.config_revision = delivery->state->config_revision;
            effect.plugin_id = delivery->state->plugin_id.c_str();
            effect.actual_enabled = delivery->state->actual_enabled ? 1 : 0;
            effect.config_json = delivery->state->config_json.data();
            effect.config_json_size = static_cast<uint32_t>(delivery->state->config_json.size());
            effect.desired_enabled = delivery->state->desired_enabled ? 1 : 0;
            RuntimeSubscription* previous = active_subscription_;
            active_subscription_ = subscription.get();
            try { subscription->observer(subscription->context, &effect); }
            catch (...) { QOL_LOG_ERROR(QolDomain::kCore, "runtime effect observer threw"); }
            active_subscription_ = previous;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --subscription->in_flight;
            delivery->complete = true;
        }
        changed_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<std::string, std::shared_ptr<const RuntimeState>> states_;
    std::unordered_map<uint64_t, std::shared_ptr<RuntimeSubscription>> subscriptions_;
    std::deque<std::shared_ptr<RuntimeDelivery>> deliveries_;
    uint64_t event_revision_ = 0;
    uint64_t next_subscription_id_ = 1;
    bool dispatching_ = false;
    static thread_local RuntimeEffectsHub* active_hub_;
    static thread_local RuntimeSubscription* active_subscription_;
};

thread_local RuntimeEffectsHub* RuntimeEffectsHub::active_hub_ = nullptr;
thread_local RuntimeSubscription* RuntimeEffectsHub::active_subscription_ = nullptr;

RuntimeEffectsHub& runtime_hub() {
    static RuntimeEffectsHub hub;
    return hub;
}

class RuntimeOwnerLease {
public:
    explicit RuntimeOwnerLease(contracts::QolOwnerToken token)
        : status(owner_namespace_call_begin(token, &call)) {}
    ~RuntimeOwnerLease() { owner_namespace_call_end(&call); }
    OwnerNamespaceCall call;
    QolResult status;
};

QolResult subscribe_self(void*, contracts::QolOwnerToken token,
                         contracts::QolPluginRuntimeObserver observer, void* context,
                         uint64_t* out_id) {
    RuntimeOwnerLease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    return runtime_hub().subscribe(lease.call.plugin_id.c_str(), lease.call.plugin_id.c_str(),
                                   observer, context, out_id);
}

QolResult unsubscribe_self(void*, contracts::QolOwnerToken token, uint64_t id) {
    RuntimeOwnerLease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    return runtime_hub().unsubscribe(id, lease.call.plugin_id.c_str(), false);
}

QolResult security_query(void*, uint8_t* out_enabled) {
    if (out_enabled == nullptr) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out_enabled = 0;
    if (core_settings_config_service().state() == CoreSettingsConfigServiceState::kUninitialized)
        return contracts::error(kQolErrNotReady, nullptr, 0);
    uint8_t enabled = 0;
    const QolResult read = production_feature_controller().read_feature(
        kSecurityConfigOwner, "op.enabled", &enabled);
    if (!contracts::succeeded(read)) return read;
    *out_enabled = enabled != 0 ? 1 : 0;
    return contracts::ok();
}

void runtime_config_observer(void*, const FeatureControlSnapshot& snapshot) {
    runtime_effects_publish_config_internal(snapshot);
}

struct ProductionFeatures {
    FeatureDirectory directory;
    FeatureController controller{core_settings_config_service()};
    FeatureReconciler reconciler{controller};
    std::vector<std::vector<contracts::QolFeatureCatalogItem>> catalog_items;
    std::vector<contracts::QolFeatureCatalogPlugin> catalog;
    bool sealed = false;
    std::atomic<bool> catalog_ready{false};
    ProductionFeatures() {
        controller.add_state_observer(&feature_platform_publish, nullptr);
        controller.add_state_observer(&runtime_config_observer, nullptr);
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

QolResult read_categories(void*, const contracts::FeatureCategoryDefinition** out,
                          uint32_t* count) {
    if (out != nullptr) *out = nullptr;
    if (count != nullptr) *count = 0;
    if (out == nullptr || count == nullptr) return error(kQolErrInvalidArgument);
    *out = kFeatureCategories;
    *count = static_cast<uint32_t>(std::size(kFeatureCategories));
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
        &read_config, &set_config, &read_actual, &read_categories};
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
        if (plugin.plugin_id == "settings") continue;

        self.catalog_items.emplace_back();
        auto& items = self.catalog_items.back();
        items.reserve(plugin.features.size());
        for (const auto& feature : plugin.features) {
            items.push_back({feature.id, feature.default_enabled, feature.category,
                             feature.type, feature.name, feature.description,
                             feature.action_label, feature.display_priority});
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
    if (sdk.abi.major != contracts::kHostSdkMajor || sdk.abi.minor < 26 ||
        contracts::feature_if_available(&sdk) == nullptr ||
        sdk.abi.size < offsetof(contracts::QolHostSdk, feature_control) +
                       sizeof(sdk.feature_control)) return false;
    sdk.feature_control = &production_feature_control_api();
    return contracts::feature_control_if_available(&sdk) == &production_feature_control_api();
}

const contracts::QolRuntimeEffectsApi& production_runtime_effects_api() {
    static const contracts::QolRuntimeEffectsApi api{
        sizeof(contracts::QolRuntimeEffectsApi), nullptr, &subscribe_self, &unsubscribe_self};
    return api;
}

const contracts::QolSecurityApi& production_security_api() {
    static const contracts::QolSecurityApi api{
        sizeof(contracts::QolSecurityApi), nullptr, &security_query};
    return api;
}

bool production_feature_bind_runtime_sdk(contracts::QolHostSdk& sdk) {
    constexpr std::size_t end = offsetof(contracts::QolHostSdk, security) + sizeof(sdk.security);
    if (sdk.abi.major != contracts::kHostSdkMajor || sdk.abi.minor < 24 ||
        sdk.abi.size < end) return false;
    sdk.runtime_effects = &production_runtime_effects_api();
    sdk.security = &production_security_api();
    return true;
}

QolResult runtime_effects_subscribe_trusted(
    const char* plugin_id, contracts::QolPluginRuntimeObserver observer, void* context,
    uint64_t* out_id) {
    return runtime_hub().subscribe(plugin_id, nullptr, observer, context, out_id);
}

QolResult runtime_effects_unsubscribe_trusted(uint64_t id) {
    return runtime_hub().unsubscribe(id, nullptr, true);
}

void runtime_effects_publish_actual(const char* plugin_id, bool enabled) {
    runtime_hub().publish_actual(plugin_id, enabled);
}

void runtime_effects_unsubscribe_plugin(const char* plugin_id) {
    runtime_hub().unsubscribe_owner(plugin_id);
}

void runtime_effects_publish_config_internal(const FeatureControlSnapshot& snapshot) {
    runtime_hub().publish_config(snapshot);
}

bool security_op_enabled() {
    uint8_t enabled = 0;
    return contracts::succeeded(security_query(nullptr, &enabled)) && enabled != 0;
}

}  // namespace qol::host
