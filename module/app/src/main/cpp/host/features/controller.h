#pragma once

#include "contracts/plugin.h"
#include "host/core_settings_config_service.h"
#include <map>
#include <memory>

namespace qol::host {

// Stage3 registers every product descriptor here before selecting the new Store model.
// No implicit feature is created for old descriptors.
class FeatureDirectory {
public:
    ConfigStoreResult add(const contracts::PluginDescriptor& descriptor);
    ConfigStoreResult add_host_owner(const contracts::PluginDescriptor& descriptor);
    const std::vector<std::string>& host_owners() const { return host_owners_; }
    const std::vector<PluginFeatureSeed>& definitions() const { return definitions_; }
private:
    std::vector<PluginFeatureSeed> definitions_;
    std::vector<std::string> host_owners_;
};

// The registry implementation must admit a main-thread command and guard actual active
// state/owner lease there. It must not execute the metadata callback on the caller thread.
using FeatureEntryAdmission = QolResult (*)(void*, const char* plugin, const char* feature);
using FeatureReceiptObserver = void (*)(void*, const contracts::FeatureReceipt&) noexcept;

// Immutable durable projection, published by the worker after Store effects admission.
// Frame readers never acquire the Store mutex held by atomic file persistence.

class FeatureController {
public:
    explicit FeatureController(CoreSettingsConfigService& service) : service_(service) {}
    ~FeatureController();
    FeatureController(const FeatureController&) = delete;
    FeatureController& operator=(const FeatureController&) = delete;

    // Host bootstrap only, never a UI/game callback. The worker handles subsequent writes.
    ConfigStoreResult initialize(const FeatureDirectory&, const std::string& directory,
                                 FeatureDesiredObserver pending, void* context);
    QolResult grant_admin(contracts::QolOwnerToken);
    void bind_entry_admission(FeatureEntryAdmission, void* context);
    void bind_state_observer(FeatureControlObserver, void* context);
    void add_state_observer(FeatureControlObserver, void* context);
    bool host_owned(const char*) const;
    QolResult read_registered(char*, uint32_t, uint32_t*) const;
    const contracts::QolFeatureApi& api() const { return api_; }
    const std::vector<PluginFeatureSeed>& catalog() const { return definitions_; }
    ConfigStoreResult desired(std::vector<PluginDesiredSnapshot>* out) const;
    QolResult read_feature(const char*, const char*, uint8_t*) const;
    QolResult read_config(const char*, const char*, char*, uint32_t, uint32_t*) const;

    QolResult request_set_feature(const char*, const char*, uint8_t, contracts::FeatureReceipt*);
    QolResult request_set_config(const char*, const char*, const char*, uint32_t, contracts::FeatureReceipt*);
    QolResult request_set_config_if_revision(const char*, const char*, const char*, uint32_t,
                                             uint64_t, contracts::FeatureReceipt*);
    QolResult request_desired(const char*, uint8_t, contracts::FeatureReceipt*);
    QolResult receipt(uint64_t, contracts::FeatureReceipt*) const;
    // Host-private one-shot notification. A terminal receipt replays inline with no live token.
    QolResult subscribe_receipt(uint64_t request_id, FeatureReceiptObserver, void*, uint64_t*);
    QolResult unsubscribe_receipt(uint64_t subscription_id);
    QolResult invoke_entry(const char*, const char*);
    // Loader shutdown only, not a frame/lifecycle callback. Drains the worker before
    // destroying its pending observer context; no inactive plugin token is involved.
    void shutdown() { stop(); }

private:
    struct Request {
        std::string plugin;
        FeatureConfigField field;
        std::string key;
        std::string json;
        uint64_t id;
        bool has_expected_revision = false;
        uint64_t expected_revision = 0;
    };
    struct ReceiptRecord {
        contracts::FeatureReceipt value{};
        uint64_t owner = 0;
    };
    struct ReceiptObserverRecord {
        uint64_t id = 0;
        uint64_t request_id = 0;
        FeatureReceiptObserver observer = nullptr;
        void* context = nullptr;
        uint32_t in_flight = 0;
        bool cancelled = false;
    };
    // Authority is captured under an owner lease at admission. Durable config requests
    // already admitted remain valid after stop; worker notifications hold no owner lease.
    QolResult authorize(contracts::QolOwnerToken, const char* target, bool admin_only = false) const;
    QolResult enqueue(const char*, FeatureConfigField, const char*, const std::string&,
                      uint64_t owner, contracts::FeatureReceipt*,
                      bool has_expected_revision = false, uint64_t expected_revision = 0);
    QolResult read_namespace(const char*, char*, uint32_t, uint32_t*) const;
    QolResult read_value(const char*, const char*, const char*, JsonValue*) const;
    const PluginFeatureSeed* find_plugin(const char*) const;
    void run();
    void stop();
    static void desired_event(void*, const FeatureControlSnapshot&);
    CoreSettingsConfigService& service_;
    std::vector<PluginFeatureSeed> definitions_;
    std::vector<std::string> host_owners_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Request> queue_;
    std::map<uint64_t, ReceiptRecord> receipts_;
    std::map<uint64_t, std::shared_ptr<ReceiptObserverRecord>> receipt_observers_;
    uint64_t next_receipt_observer_id_ = 1;
    std::vector<uint64_t> admin_tokens_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
    uint64_t next_id_ = 1;
    FeatureEntryAdmission entry_ = nullptr;
    void* entry_context_ = nullptr;
    FeatureDesiredObserver desired_observer_ = nullptr;
    void* desired_context_ = nullptr;
    FeatureControlObserver state_observer_ = nullptr;
    void* state_context_ = nullptr;
    std::vector<std::pair<FeatureControlObserver, void*>> state_observers_;
    FeatureControlSnapshot published_;
    static thread_local FeatureController* active_receipt_dispatcher_;

    static QolResult api_namespace(void*, contracts::QolOwnerToken, const char*, char*, uint32_t, uint32_t*);
    static QolResult api_feature(void*, contracts::QolOwnerToken, const char*, const char*, uint8_t*);
    static QolResult api_set_feature(void*, contracts::QolOwnerToken, const char*, const char*, uint8_t, contracts::FeatureReceipt*);
    static QolResult api_config(void*, contracts::QolOwnerToken, const char*, const char*, char*, uint32_t, uint32_t*);
    static QolResult api_set_config(void*, contracts::QolOwnerToken, const char*, const char*, const char*, uint32_t, contracts::FeatureReceipt*);
    static QolResult api_receipt(void*, contracts::QolOwnerToken, uint64_t, contracts::FeatureReceipt*);
    static QolResult api_catalog(void*, contracts::QolOwnerToken, char*, uint32_t, uint32_t*);
    static QolResult api_desired(void*, contracts::QolOwnerToken, const char*, uint8_t*, uint8_t*);
    static QolResult api_set_desired(void*, contracts::QolOwnerToken, const char*, uint8_t, contracts::FeatureReceipt*);
    static QolResult api_entry(void*, contracts::QolOwnerToken, const char*, const char*);
    contracts::QolFeatureApi api_{sizeof(contracts::QolFeatureApi), this, &api_namespace,
        &api_feature, &api_set_feature, &api_config, &api_set_config, &api_receipt,
        &api_catalog, &api_desired, &api_set_desired, &api_entry};
};
}  // namespace qol::host
