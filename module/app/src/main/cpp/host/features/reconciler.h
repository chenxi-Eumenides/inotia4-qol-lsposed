#pragma once

#include "host/features/controller.h"
#include "host/plugin_registry.h"

namespace qol::host {

// Loader-lifetime new-model adapter. The worker only delivers immutable desired data;
// step() is called at a logical frame safe point on the bound main thread.
class FeatureReconciler {
public:
    explicit FeatureReconciler(FeatureController& controller);
    ~FeatureReconciler();
    FeatureReconciler(const FeatureReconciler&) = delete;
    FeatureReconciler& operator=(const FeatureReconciler&) = delete;
    static void pending_desired(void*, uint64_t, const std::vector<PluginDesiredSnapshot>&);
    static void pending_state(void*, const FeatureControlSnapshot&);
    bool bind(const HostDeps&);
    void notify_dependency_changed();
    uint32_t step();
    uint64_t applied_revision() const { return revision_; }
    int32_t last_callback_error() const { return callback_error_; }
    QolResult admit_entry(const char* plugin, const char* feature);
    using AvailabilityProbe = QolResult (*)(contracts::PluginContext&, void*);
    QolResult probe_availability(const char*, const char*, AvailabilityProbe, void*);

private:
    static bool request_desired(void*, const char*, bool);
    static QolResult entry_admission(void*, const char*, const char*);
    static bool wanted(void*, const char*);
    bool business_ready(const char*, const char*, contracts::PluginContext*);
    void notify_toggles();
    FeatureController& controller_;
    std::mutex mutex_;
    std::vector<PluginDesiredSnapshot> pending_;
    std::vector<std::pair<std::string, JsonValue>> pending_namespaces_;
    uint64_t pending_revision_ = 0;
    bool dependency_dirty_ = false;
    std::deque<std::pair<std::string, std::string>> entries_;
    std::map<std::string, PluginDesiredSnapshot> desired_;
    std::map<std::string, bool> eligible_;
    std::map<std::pair<std::string, std::string>, bool> feature_values_;
    std::map<std::string, uint64_t> attempted_;
    // Previous frame's per-plugin dependency readiness; a false→true edge is a one-shot
    // dependency event equivalent to notify_dependency_changed().
    std::map<std::string, bool> dependency_ready_;
    std::map<std::pair<std::string, std::string>, bool> notified_;
    std::map<std::string, contracts::QolOwnerToken> notified_tokens_;
    std::thread::id main_thread_;
    bool bound_ = false;
    uint64_t revision_ = 0;
    uint64_t batch_ = 0;
    int32_t callback_error_ = 0;
};
}  // namespace qol::host
