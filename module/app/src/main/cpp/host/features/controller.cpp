#include "controller.h"
#include "host/owner_identity.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace qol::host {
namespace {
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
    auto candidate = definitions_;
    candidate.push_back({descriptor.id, {descriptor.features, descriptor.features + descriptor.feature_count}});
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
    const std::string& json, uint64_t owner, contracts::FeatureReceipt* out) {
    using namespace contracts;
    if (out) *out = {};
    auto* plugin = find_plugin(id);
    if (!out || !plugin || !key) return error(kQolErrInvalidArgument);
    JsonValue value;
    auto parsed = features::core_settings::parse_json_document(json, &value);
    if (!parsed.ok()) return result(parsed);
    if (field != FeatureConfigField::kConfig && value.kind != JsonValue::Kind::kBoolean) return error(kQolErrInvalidArgument);
    if (field == FeatureConfigField::kDesiredEnable && host_owned(id)) return error(kQolErrUnsupported);
    if (field == FeatureConfigField::kConfig && config_validator_) {
        const auto validated = config_validator_(id, key, value);
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
    queue_.push_back({id, field, key, json, out->request_id});
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
            const auto committed = service_.commit_feature_model(request.plugin, request.field, request.key, request.json);
            completed.revision = committed.revision;
            completed.error_code = result(committed.result).code;
            if (committed.committed) {
                completed.persistence = FeaturePersistence::kPersisted;
                completed.actual = FeatureActual::kPending;
                completed.error_code = committed.apply_error_code;
            }
        } catch (...) {
            // Receipt exposes failure and the worker continues servicing later admissions.
            completed.error_code = kQolErrInternal;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        receipts_.at(request.id).value = completed;
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
