#include "controller.h"
#include "host/owner_identity.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>

namespace qol::host {
thread_local FeatureController* FeatureController::active_receipt_dispatcher_ = nullptr;
namespace {
constexpr std::size_t kMaxReceiptObservers = 256;
QolResult error(int32_t code) { return contracts::error(code, nullptr, 0); }
QolResult admission(QolResult status, contracts::FeatureReceipt* out) {
    if (!contracts::succeeded(status) && out) {
        *out = {};
        out->error_code = status.code;
    }
    return status;
}
QolResult result(const ConfigStoreResult& value) {
    using E = features::core_settings::ConfigStoreError;
    if (value.ok()) return contracts::ok();
    if (value.error == E::kNotReady) return error(kQolErrNotReady);
    if (value.error == E::kNotFound) return error(kQolErrNotFound);
    if (value.error == E::kPersistence) return error(kQolErrInternal);
    return error(kQolErrInvalidArgument);
}
const JsonValue* member(const JsonValue& value, const std::string& key) {
    for (const auto& item : value.object) if (item.first == key) return &item.second;
    return nullptr;
}
QolResult copy_json(const JsonValue& value, char* buffer, uint32_t capacity, uint32_t* size) {
    if (!size || (capacity && !buffer)) return error(kQolErrInvalidArgument);
    *size = 0;
    auto json = features::core_settings::serialize_json_value(value);
    if (json.size() > capacity) return error(kQolErrInvalidArgument);
    std::memcpy(buffer, json.data(), json.size());
    *size = static_cast<uint32_t>(json.size());
    return contracts::ok();
}
class Lease {
public:
    explicit Lease(contracts::QolOwnerToken token) : status(owner_namespace_call_begin(token, &call)) {}
    ~Lease() { owner_namespace_call_end(&call); }
    OwnerNamespaceCall call;
    QolResult status;
};
JsonValue string(const char* text) {
    JsonValue value;
    value.kind = JsonValue::Kind::kString;
    value.scalar = text;
    return value;
}
}  // namespace

ConfigStoreResult FeatureDirectory::add(const contracts::PluginDescriptor& descriptor) {
    if (!descriptor.id || !descriptor.features || !descriptor.feature_count || descriptor.feature_count > 1024)
        return {features::core_settings::ConfigStoreError::kValidation, "missing/nonzero feature definitions required", false};
    if ((descriptor.http_endpoint_count != 0 && descriptor.http_endpoints == nullptr) ||
        descriptor.http_endpoint_count > 4096) {
        return {features::core_settings::ConfigStoreError::kValidation,
                "invalid descriptor tail", false};
    }
    for (uint32_t index = 0; index < descriptor.http_endpoint_count; ++index) {
        const auto& endpoint = descriptor.http_endpoints[index];
        if (endpoint.path == nullptr || endpoint.path[0] != '/' || endpoint.route_owner == nullptr ||
            endpoint.route_owner[0] == '\0' || endpoint.op_required > 1 ||
            endpoint.lease_required > 1) {
            return {features::core_settings::ConfigStoreError::kValidation,
                    "invalid HTTP endpoint descriptor", false};
        }
        const bool get = endpoint.method == contracts::HttpMethod::kGet;
        const bool post = endpoint.method == contracts::HttpMethod::kPost;
        if ((!get && !post) ||
            (get && (endpoint.kind != contracts::HttpHandlerKind::kQuery ||
                     endpoint.query_handler == nullptr || endpoint.command_handler != nullptr)) ||
            (post && (endpoint.kind != contracts::HttpHandlerKind::kCommand ||
                      endpoint.command_handler == nullptr || endpoint.query_handler != nullptr))) {
            return {features::core_settings::ConfigStoreError::kValidation,
                    "HTTP method, kind and handler mismatch", false};
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            const auto& other = descriptor.http_endpoints[previous];
            if (other.method == endpoint.method && std::strcmp(other.path, endpoint.path) == 0) {
                return {features::core_settings::ConfigStoreError::kValidation,
                        "duplicate HTTP endpoint", false};
            }
        }
    }
    auto candidate = definitions_;
    candidate.push_back({descriptor.id,
                         {descriptor.features, descriptor.features + descriptor.feature_count},
                         descriptor.validate_config});
    features::core_settings::ConfigStore validation;
    auto checked = validation.prepare_features(candidate);
    if (checked.ok()) definitions_ = std::move(candidate);
    return checked;
}

FeatureController::~FeatureController() { stop(); }

ConfigStoreResult FeatureDirectory::add_host_owner(const contracts::PluginDescriptor& descriptor) {
    auto result = add(descriptor);
    if (result.ok()) host_owners_.emplace_back(descriptor.id);
    return result;
}
bool FeatureController::host_owned(const char* id) const {
    return id && std::find(host_owners_.begin(), host_owners_.end(), id) != host_owners_.end();
}
QolResult FeatureController::read_registered(char* buffer, uint32_t capacity, uint32_t* size) const {
    JsonValue root;
    root.kind = JsonValue::Kind::kObject;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!published_.revision) return error(kQolErrNotReady);
        root.object = published_.namespaces;
        for (auto& item : root.object) {
            for (const auto& desired : published_.desired) {
                if (desired.plugin_id != item.first) continue;
                JsonValue enabled;
                enabled.kind = JsonValue::Kind::kBoolean;
                enabled.boolean = desired.desired_enable;
                // 投影键序与磁盘 schema 一致（enable,features,config）；read_feature_namespace
                // 只回读 features/config，故 enable 插到最前而不是追加到末尾。
                item.second.object.insert(item.second.object.begin(),
                                          std::make_pair(std::string("enable"), enabled));
            }
        }
    }
    return copy_json(root, buffer, capacity, size);
}

ConfigStoreResult FeatureController::initialize(const FeatureDirectory& directory,
    const std::string& path, FeatureDesiredObserver pending, void* context) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (running_ || stopping_ || !definitions_.empty()) return {features::core_settings::ConfigStoreError::kValidation, "controller already initialized", false};
    auto prepared = service_.prepare_features(directory.definitions(), nullptr, this, &desired_event);
    if (!prepared.ok()) return prepared;
    desired_observer_ = pending;
    desired_context_ = context;
    definitions_ = directory.definitions();
    host_owners_ = directory.host_owners();
    lock.unlock();
    auto initialized = service_.initialize(path);
    auto seeded = service_.seed_features();
    lock.lock();
    running_ = true;
    worker_ = std::thread(&FeatureController::run, this);
    return initialized.ok() ? seeded.result : initialized;
}

const PluginFeatureSeed* FeatureController::find_plugin(const char* id) const {
    if (!id) return nullptr;
    for (const auto& item : definitions_) if (item.plugin_id == id) return &item;
    return nullptr;
}

QolResult FeatureController::grant_admin(contracts::QolOwnerToken token) {
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(admin_tokens_.begin(), admin_tokens_.end(), token) == admin_tokens_.end())
        admin_tokens_.push_back(token);
    return contracts::ok();
}

QolResult FeatureController::authorize(contracts::QolOwnerToken token, const char* target, bool admin_only) const {
    // Callers retain the lease for the entire read or admission, never for worker effects.
    std::string id;
    auto verified = owner_plugin_id_for_token(token, &id);
    if (!contracts::succeeded(verified)) return verified;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool admin = std::find(admin_tokens_.begin(), admin_tokens_.end(), token) != admin_tokens_.end();
    if (admin || (!admin_only && target && id == target)) return contracts::ok();
    return error(kQolErrBlocked);
}

ConfigStoreResult FeatureController::desired(std::vector<PluginDesiredSnapshot>* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out || !published_.revision)
        return {features::core_settings::ConfigStoreError::kNotReady, "durable projection unavailable", false};
    *out = published_.desired;
    return {};
}

void FeatureController::bind_state_observer(FeatureControlObserver observer, void* context) {
    FeatureControlSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_observer_ = observer;
        state_context_ = context;
        snapshot = published_;
    }
    if (observer && snapshot.revision) observer(context, snapshot);
}
void FeatureController::add_state_observer(FeatureControlObserver observer, void* context) {
    if (!observer) return;
    FeatureControlSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_observers_.emplace_back(observer, context);
        snapshot = published_;
    }
    if (snapshot.revision) observer(context, snapshot);
}

void FeatureController::desired_event(void* context, const FeatureControlSnapshot& snapshot) {
    auto& self = *static_cast<FeatureController*>(context);
    FeatureControlObserver observer;
    void* observer_context;
    FeatureDesiredObserver pending;
    void* pending_context;
    std::vector<std::pair<FeatureControlObserver, void*>> observers;
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.published_ = snapshot;
        observer = self.state_observer_;
        observer_context = self.state_context_;
        pending = self.desired_observer_;
        pending_context = self.desired_context_;
        observers = self.state_observers_;
    }
    if (observer) observer(observer_context, snapshot);
    if (pending) pending(pending_context, snapshot.revision, snapshot.desired);
    for (const auto& item : observers) item.first(item.second, snapshot);
}

QolResult FeatureController::read_feature(const char* id, const char* key, uint8_t* out) const {
    if (!out) return error(kQolErrInvalidArgument);
    JsonValue value;
    auto read = read_value(id, "features", key, &value);
    if (contracts::succeeded(read)) *out = value.boolean;
    return read;
}

QolResult FeatureController::read_config(const char* id, const char* key, char* buffer,
                                        uint32_t capacity, uint32_t* size) const {
    JsonValue value;
    auto read = read_value(id, "config", key, &value);
    return contracts::succeeded(read) ? copy_json(value, buffer, capacity, size) : read;
}

QolResult FeatureController::read_namespace(const char* id, char* buffer, uint32_t capacity, uint32_t* size) const {
    if (!id) return error(kQolErrInvalidArgument);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& value : published_.namespaces) {
        if (value.first == id) return copy_json(value.second, buffer, capacity, size);
    }
    return error(kQolErrNotReady);
}

QolResult FeatureController::read_value(const char* id, const char* section, const char* key, JsonValue* out) const {
    if (!id || !key || !out) return error(kQolErrInvalidArgument);
    if (std::strcmp(section, "features") == 0) {
        auto* plugin = find_plugin(id);
        if (!plugin || std::none_of(plugin->features.begin(), plugin->features.end(), [&](const auto& f) { return std::strcmp(f.id, key) == 0; })) return error(kQolErrNotFound);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const JsonValue* object = nullptr;
    for (const auto& item : published_.namespaces) if (item.first == id) { object = &item.second; break; }
    if (!object) return error(kQolErrNotReady);
    const auto* values = member(*object, section);
    const auto* value = values ? member(*values, key) : nullptr;
    if (!value) return error(kQolErrNotFound);
    *out = *value;
    return contracts::ok();
}

QolResult FeatureController::enqueue(const char* id, FeatureConfigField field, const char* key,
    const std::string& json, uint64_t owner, contracts::FeatureReceipt* out,
    bool has_expected_revision, uint64_t expected_revision) {
    using namespace contracts;
    if (out) *out = {};
    auto* plugin = find_plugin(id);
    if (!out || !plugin || !key) return error(kQolErrInvalidArgument);
    JsonValue value;
    auto parsed = features::core_settings::parse_json_document(json, &value);
    if (!parsed.ok()) return result(parsed);
    if (field != FeatureConfigField::kConfig && value.kind != JsonValue::Kind::kBoolean) return error(kQolErrInvalidArgument);
    if (field == FeatureConfigField::kDesiredEnable && host_owned(id)) return error(kQolErrUnsupported);
    if (field == FeatureConfigField::kConfig && plugin->validate_config != nullptr) {
        const auto validated = plugin->validate_config(key, json.data(),
                                                       static_cast<uint32_t>(json.size()));
        if (!contracts::succeeded(validated)) return validated;
    }
    if (field == FeatureConfigField::kFeature && std::none_of(plugin->features.begin(), plugin->features.end(), [&](const auto& f) { return std::strcmp(f.id, key) == 0; })) return error(kQolErrNotFound);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stopping_) return error(kQolErrNotReady);
    if (queue_.size() >= 256) return error(kQolErrBusy);
    while (receipts_.size() >= 512) {
        auto old = std::find_if(receipts_.begin(), receipts_.end(), [](const auto& item) { return item.second.value.persistence != FeaturePersistence::kPending; });
        if (old == receipts_.end()) return error(kQolErrBusy);
        receipts_.erase(old);
    }
    *out = {next_id_++, 0, FeatureAdmission::kAdmitted, FeaturePersistence::kPending, FeatureActual::kNotSubmitted, 0};
    receipts_.emplace(out->request_id, ReceiptRecord{*out, owner});
    queue_.push_back({id, field, key, json, out->request_id,
                      has_expected_revision, expected_revision});
    changed_.notify_one();
    return ok();
}

QolResult FeatureController::request_set_feature(const char* id, const char* key, uint8_t value, contracts::FeatureReceipt* out) {
    if (value > 1) return admission(error(kQolErrInvalidArgument), out);
    return admission(enqueue(id, FeatureConfigField::kFeature, key, value ? "true" : "false", 0, out), out);
}
QolResult FeatureController::request_set_config(const char* id, const char* key, const char* json, uint32_t size, contracts::FeatureReceipt* out) {
    if (!json && size) return admission(error(kQolErrInvalidArgument), out);
    return admission(enqueue(id, FeatureConfigField::kConfig, key, std::string(json ? json : "", size), 0, out), out);
}
QolResult FeatureController::request_set_config_if_revision(
    const char* id, const char* key, const char* json, uint32_t size,
    uint64_t expected_revision, contracts::FeatureReceipt* out) {
    if (!json && size) return admission(error(kQolErrInvalidArgument), out);
    return admission(enqueue(id, FeatureConfigField::kConfig, key,
                             std::string(json ? json : "", size), 0, out, true,
                             expected_revision), out);
}
QolResult FeatureController::request_desired(const char* id, uint8_t value, contracts::FeatureReceipt* out) {
    if (value > 1) return admission(error(kQolErrInvalidArgument), out);
    return admission(enqueue(id, FeatureConfigField::kDesiredEnable, "", value ? "true" : "false", 0, out), out);
}
QolResult FeatureController::receipt(uint64_t id, contracts::FeatureReceipt* out) const {
    if (!out) return error(kQolErrInvalidArgument);
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = receipts_.find(id);
    if (found == receipts_.end()) return error(kQolErrNotFound);
    *out = found->second.value;
    return contracts::ok();
}

QolResult FeatureController::subscribe_receipt(uint64_t request_id,
                                               FeatureReceiptObserver observer,
                                               void* context, uint64_t* out_subscription) {
    if (out_subscription != nullptr) *out_subscription = 0;
    if (request_id == 0 || observer == nullptr || out_subscription == nullptr)
        return error(kQolErrInvalidArgument);

    contracts::FeatureReceipt terminal{};
    bool replay_terminal = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopping_) return error(kQolErrNotReady);
        const auto receipt = receipts_.find(request_id);
        if (receipt == receipts_.end()) return error(kQolErrNotFound);
        if (receipt->second.value.persistence != contracts::FeaturePersistence::kPending) {
            terminal = receipt->second.value;
            replay_terminal = true;
        } else {
            if (receipt_observers_.size() >= kMaxReceiptObservers)
                return error(kQolErrBusy);
            uint64_t id = next_receipt_observer_id_;
            while (id == 0 || receipt_observers_.find(id) != receipt_observers_.end()) {
                ++id;
                if (id == 0) id = 1;
            }
            next_receipt_observer_id_ = id + 1;
            if (next_receipt_observer_id_ == 0) next_receipt_observer_id_ = 1;

            std::shared_ptr<ReceiptObserverRecord> subscription;
            try {
                subscription = std::make_shared<ReceiptObserverRecord>();
                subscription->id = id;
                subscription->request_id = request_id;
                subscription->observer = observer;
                subscription->context = context;
                receipt_observers_.emplace(id, subscription);
            } catch (const std::bad_alloc&) {
                return error(kQolErrInternal);
            }
            *out_subscription = id;
            return contracts::ok();
        }
    }

    // A terminal replay has no live subscription token; the callback owns this receipt snapshot
    // for its full duration and runs without the controller mutex.
    if (replay_terminal) observer(context, terminal);
    return contracts::ok();
}

QolResult FeatureController::unsubscribe_receipt(uint64_t subscription_id) {
    if (subscription_id == 0) return error(kQolErrInvalidArgument);
    std::unique_lock<std::mutex> lock(mutex_);
    auto found = receipt_observers_.find(subscription_id);
    if (found == receipt_observers_.end()) return error(kQolErrNotFound);
    const auto subscription = found->second;
    subscription->cancelled = true;
    if (active_receipt_dispatcher_ == this) {
        if (subscription->in_flight == 0) receipt_observers_.erase(found);
        return contracts::ok();
    }
    if (subscription->in_flight != 0) {
        changed_.wait(lock, [&] { return subscription->in_flight == 0; });
        found = receipt_observers_.find(subscription_id);
        if (found != receipt_observers_.end() && found->second == subscription)
            receipt_observers_.erase(found);
    } else {
        receipt_observers_.erase(found);
    }
    return contracts::ok();
}

void FeatureController::bind_entry_admission(FeatureEntryAdmission callback, void* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    entry_ = callback;
    entry_context_ = context;
}
QolResult FeatureController::invoke_entry(const char* id, const char* key) {
    auto* plugin = find_plugin(id);
    if (!plugin || !key) return error(kQolErrInvalidArgument);
    auto found = std::find_if(plugin->features.begin(), plugin->features.end(), [&](const auto& f) { return std::strcmp(f.id, key) == 0; });
    if (found == plugin->features.end() || found->type != contracts::FeatureType::kEntry) return error(kQolErrInvalidArgument);
    JsonValue enabled;
    auto read = read_value(id, "features", key, &enabled);
    if (!contracts::succeeded(read)) return read;
    if (!enabled.boolean) return error(kQolErrBlocked);
    FeatureEntryAdmission callback;
    void* context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = entry_;
        context = entry_context_;
    }
    if (!callback) return error(kQolErrNotReady);
    try { return callback(context, id, key); }
    catch (...) { return error(kQolErrInternal); }
}

void FeatureController::run() {
    using namespace contracts;
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) return;
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        FeatureReceipt completed{request.id, 0, FeatureAdmission::kAdmitted, FeaturePersistence::kFailed, FeatureActual::kNotSubmitted, kQolErrInternal};
        try {
            const auto committed = request.has_expected_revision
                ? service_.commit_feature_model_if_revision(request.plugin, request.key,
                                                            request.json, request.expected_revision)
                : service_.commit_feature_model(request.plugin, request.field,
                                                request.key, request.json);
            completed.revision = committed.revision;
            completed.error_code = committed.conflict ? kQolErrConflict : result(committed.result).code;
            if (committed.committed) {
                completed.persistence = FeaturePersistence::kPersisted;
                completed.actual = FeatureActual::kPending;
                completed.error_code = committed.apply_error_code;
            }
        } catch (...) {
            // Receipt exposes failure and the worker continues servicing later admissions.
            completed.error_code = kQolErrInternal;
        }
        std::array<std::shared_ptr<ReceiptObserverRecord>, kMaxReceiptObservers> observers{};
        std::size_t observer_count = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            receipts_.at(request.id).value = completed;
            for (const auto& item : receipt_observers_) {
                const auto& subscription = item.second;
                if (subscription->request_id != request.id || subscription->cancelled) continue;
                ++subscription->in_flight;
                observers[observer_count++] = subscription;
            }
        }
        for (std::size_t index = 0; index < observer_count; ++index) {
            const auto& subscription = observers[index];
            bool invoke = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                invoke = !subscription->cancelled;
            }
            if (invoke) {
                FeatureController* previous = active_receipt_dispatcher_;
                active_receipt_dispatcher_ = this;
                subscription->observer(subscription->context, completed);
                active_receipt_dispatcher_ = previous;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --subscription->in_flight;
                const auto found = receipt_observers_.find(subscription->id);
                if (found != receipt_observers_.end() && found->second == subscription)
                    receipt_observers_.erase(found);
            }
            changed_.notify_all();
        }
    }
}
void FeatureController::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}

QolResult FeatureController::api_namespace(void* context, contracts::QolOwnerToken token, const char* id, char* buffer, uint32_t cap, uint32_t* size) {
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    auto auth = self.authorize(token, id);
    return contracts::succeeded(auth) ? self.read_namespace(id, buffer, cap, size) : auth;
}
QolResult FeatureController::api_feature(void* context, contracts::QolOwnerToken token, const char* id, const char* key, uint8_t* out) {
    if (!out) return error(kQolErrInvalidArgument);
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    auto auth = self.authorize(token, id);
    if (!contracts::succeeded(auth)) return auth;
    return self.read_feature(id, key, out);
}
QolResult FeatureController::api_set_feature(void* context, contracts::QolOwnerToken token, const char* id, const char* key, uint8_t value, contracts::FeatureReceipt* out) {
    if (value > 1) return admission(error(kQolErrInvalidArgument), out);
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return admission(lease.status, out);
    auto auth = self.authorize(token, id);
    return admission(contracts::succeeded(auth) ? self.enqueue(id, FeatureConfigField::kFeature, key, value ? "true" : "false", token, out) : auth, out);
}
QolResult FeatureController::api_config(void* context, contracts::QolOwnerToken token, const char* id, const char* key, char* buffer, uint32_t cap, uint32_t* size) {
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    auto auth = self.authorize(token, id);
    if (!contracts::succeeded(auth)) return auth;
    return self.read_config(id, key, buffer, cap, size);
}
QolResult FeatureController::api_set_config(void* context, contracts::QolOwnerToken token, const char* id, const char* key, const char* json, uint32_t size, contracts::FeatureReceipt* out) {
    if (!json && size) return admission(error(kQolErrInvalidArgument), out);
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return admission(lease.status, out);
    auto auth = self.authorize(token, id);
    return admission(contracts::succeeded(auth) ? self.enqueue(id, FeatureConfigField::kConfig, key, std::string(json ? json : "", size), token, out) : auth, out);
}
QolResult FeatureController::api_receipt(void* context, contracts::QolOwnerToken token, uint64_t id, contracts::FeatureReceipt* out) {
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    if (!out) return error(kQolErrInvalidArgument);
    std::lock_guard<std::mutex> lock(self.mutex_);
    auto found = self.receipts_.find(id);
    if (found == self.receipts_.end()) return error(kQolErrNotFound);
    if (found->second.owner != token && std::find(self.admin_tokens_.begin(), self.admin_tokens_.end(), token) == self.admin_tokens_.end()) return error(kQolErrBlocked);
    *out = found->second.value;
    return contracts::ok();
}
QolResult FeatureController::api_catalog(void* context, contracts::QolOwnerToken token, char* buffer, uint32_t cap, uint32_t* size) {
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    auto auth = self.authorize(token, nullptr, true);
    if (!contracts::succeeded(auth)) return auth;
    JsonValue plugins;
    plugins.kind = JsonValue::Kind::kObject;
    for (const auto& plugin : self.definitions_) {
        JsonValue items;
        items.kind = JsonValue::Kind::kArray;
        for (const auto& f : plugin.features) {
            JsonValue item;
            item.kind = JsonValue::Kind::kObject;
            item.object = {{"id", string(f.id)}, {"name", string(f.name)}, {"description", string(f.description)},
                {"type", string(f.type == contracts::FeatureType::kEntry ? "entry" : "toggle")}};
            JsonValue number;
            number.kind = JsonValue::Kind::kNumber;
            number.scalar = std::to_string(static_cast<uint8_t>(f.category));
            item.object.emplace_back("category", number);
            JsonValue boolean;
            boolean.kind = JsonValue::Kind::kBoolean;
            boolean.boolean = f.default_enabled;
            item.object.emplace_back("default", boolean);
            items.array.push_back(std::move(item));
        }
        plugins.object.emplace_back(plugin.plugin_id, std::move(items));
    }
    return copy_json(plugins, buffer, cap, size);
}
QolResult FeatureController::api_desired(void* context, contracts::QolOwnerToken token, const char* id, uint8_t* enable, uint8_t* any) {
    if (!id || !enable || !any) return error(kQolErrInvalidArgument);
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    auto auth = self.authorize(token, id, true);
    if (!contracts::succeeded(auth)) return auth;
    std::vector<PluginDesiredSnapshot> snapshots;
    auto read = self.desired(&snapshots);
    if (!read.ok()) return result(read);
    for (const auto& item : snapshots) if (item.plugin_id == id) {
        *enable = item.desired_enable;
        *any = item.any_feature;
        return contracts::ok();
    }
    return error(kQolErrNotFound);
}
QolResult FeatureController::api_set_desired(void* context, contracts::QolOwnerToken token, const char* id, uint8_t value, contracts::FeatureReceipt* out) {
    if (value > 1) return admission(error(kQolErrInvalidArgument), out);
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return admission(lease.status, out);
    auto auth = self.authorize(token, id, true);
    return admission(contracts::succeeded(auth) ? self.enqueue(id, FeatureConfigField::kDesiredEnable, "", value ? "true" : "false", token, out) : auth, out);
}
QolResult FeatureController::api_entry(void* context, contracts::QolOwnerToken token, const char* id, const char* key) {
    auto& self = *static_cast<FeatureController*>(context);
    Lease lease(token);
    if (!contracts::succeeded(lease.status)) return lease.status;
    auto auth = self.authorize(token, id);
    return contracts::succeeded(auth) ? self.invoke_entry(id, key) : auth;
}
}  // namespace qol::host
