#include "host/settings_config_bridge.h"

#include "contracts/config_namespace.h"
#include "host/config_namespace.h"
#include "host/owner_identity.h"

#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace qol::host {
namespace {
using namespace contracts;

struct BoolSetting {
    const char* public_key;
    const char* plugin_id;
    const char* key;
    SettingsConfigApplyState fallback_apply;
};

constexpr std::array<BoolSetting, kSettingsConfigBoolCount> kSettings{{
    {"stackLimitIncrease", "stack_limit", "enable", SettingsConfigApplyState::kPendingFrame},
    {"moveMergeEnabled", "move_merge", "enable", SettingsConfigApplyState::kPendingFrame},
    {"opEnabled", "core.settings", "opEnabled", SettingsConfigApplyState::kApplied},
    {"extensionBagEnabled", "extension_bag", "enable", SettingsConfigApplyState::kPendingFrame},
    {"customRecipeEnabled", "custom_recipe", "enable", SettingsConfigApplyState::kPendingFrame},
    {"autoSellEnabled", "autosell", "enable", SettingsConfigApplyState::kPendingFrame},
    {"apiEnabled", "core.settings", "apiEnabled", SettingsConfigApplyState::kPlatformPending},
    {"debugLogEnabled", "core.settings", "debugLogEnabled", SettingsConfigApplyState::kApplied},
    {"simpleModeEnabled", "simple_mode", "enable", SettingsConfigApplyState::kPendingFrame},
    {"pushAllEnabled", "push_all", "enable", SettingsConfigApplyState::kPendingFrame},
}};

QolResult fail(int32_t code, const char* message) {
    return error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

int key_index(const char* key) {
    if (key == nullptr) return -1;
    for (uint32_t index = 0; index < kSettings.size(); ++index)
        if (std::strcmp(key, kSettings[index].public_key) == 0) return static_cast<int>(index);
    return -1;
}

bool is_bool(int32_t value) { return value == 0 || value == 1; }

class SettingsOwnerLease {
public:
    explicit SettingsOwnerLease(QolOwnerToken token)
        : result_(owner_namespace_call_begin(token, &call_)) {}
    ~SettingsOwnerLease() { owner_namespace_call_end(&call_); }
    const QolResult& result() const { return result_; }
    const char* plugin_id() const { return call_.plugin_id.c_str(); }

private:
    OwnerNamespaceCall call_;
    QolResult result_;
};

QolResult check_settings_owner(const SettingsOwnerLease& lease) {
    if (!contracts::succeeded(lease.result())) return lease.result();
    if (std::strcmp(lease.plugin_id(), "core.settings") != 0)
        return fail(kQolErrBlocked, "settings config requires core.settings owner");
    return contracts::ok();
}

class RegisteredBoolReader {
public:
    explicit RegisteredBoolReader(std::string_view json) : json_(json) {}

    bool read(const char* plugin_id, const char* field, int32_t* out, bool* found,
              bool* wrong_type) {
        if (plugin_id == nullptr || field == nullptr || out == nullptr || found == nullptr ||
            wrong_type == nullptr) return false;
        *found = false;
        *wrong_type = false;
        whitespace();
        if (!consume('{')) return false;
        whitespace();
        if (consume('}')) return true;
        for (;;) {
            std::string root_key;
            if (!read_string(&root_key)) return false;
            whitespace();
            if (!consume(':')) return false;
            whitespace();
            if (root_key == plugin_id) {
                if (!read_namespace_field(field, out, found, wrong_type, 0)) return false;
                if (*found || *wrong_type) return true;
            } else if (!skip_value(0)) {
                return false;
            }
            whitespace();
            if (consume('}')) break;
            if (!consume(',')) return false;
            whitespace();
        }
        whitespace();
        return cursor_ == json_.size();
    }

private:
    bool read_namespace_field(const char* field, int32_t* out, bool* found,
                              bool* wrong_type, unsigned depth) {
        if (depth > 64 || !consume('{')) return skip_value(depth);
        whitespace();
        if (consume('}')) return true;
        for (;;) {
            std::string key;
            if (!read_string(&key)) return false;
            whitespace();
            if (!consume(':')) return false;
            whitespace();
            if (key == field) {
                *found = true;
                if (literal("true")) { *out = 1; return true; }
                if (literal("false")) { *out = 0; return true; }
                *wrong_type = true;
                if (!skip_value(depth + 1)) return false;
            } else if (!skip_value(depth + 1)) {
                return false;
            }
            whitespace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            whitespace();
        }
    }

    bool skip_value(unsigned depth) {
        if (depth > 64) return false;
        whitespace();
        if (cursor_ >= json_.size()) return false;
        if (json_[cursor_] == '"') {
            std::string ignored;
            return read_string(&ignored);
        }
        if (json_[cursor_] == '{') {
            ++cursor_;
            whitespace();
            if (consume('}')) return true;
            for (;;) {
                std::string ignored;
                if (!read_string(&ignored)) return false;
                whitespace();
                if (!consume(':') || !skip_value(depth + 1)) return false;
                whitespace();
                if (consume('}')) return true;
                if (!consume(',')) return false;
                whitespace();
            }
        }
        if (json_[cursor_] == '[') {
            ++cursor_;
            whitespace();
            if (consume(']')) return true;
            for (;;) {
                if (!skip_value(depth + 1)) return false;
                whitespace();
                if (consume(']')) return true;
                if (!consume(',')) return false;
                whitespace();
            }
        }
        if (literal("true") || literal("false") || literal("null")) return true;
        const std::size_t begin = cursor_;
        if (json_[cursor_] == '-') ++cursor_;
        if (cursor_ < json_.size() && json_[cursor_] == '0') ++cursor_;
        else {
            const std::size_t digits = cursor_;
            while (cursor_ < json_.size() && json_[cursor_] >= '0' && json_[cursor_] <= '9') ++cursor_;
            if (digits == cursor_) return false;
        }
        if (cursor_ < json_.size() && json_[cursor_] == '.') {
            ++cursor_;
            const std::size_t digits = cursor_;
            while (cursor_ < json_.size() && json_[cursor_] >= '0' && json_[cursor_] <= '9') ++cursor_;
            if (digits == cursor_) return false;
        }
        if (cursor_ < json_.size() && (json_[cursor_] == 'e' || json_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < json_.size() && (json_[cursor_] == '+' || json_[cursor_] == '-')) ++cursor_;
            const std::size_t digits = cursor_;
            while (cursor_ < json_.size() && json_[cursor_] >= '0' && json_[cursor_] <= '9') ++cursor_;
            if (digits == cursor_) return false;
        }
        return cursor_ != begin;
    }

    bool read_string(std::string* out) {
        if (!consume('"')) return false;
        out->clear();
        while (cursor_ < json_.size()) {
            const char ch = json_[cursor_++];
            if (ch == '"') return true;
            if (ch != '\\') { out->push_back(ch); continue; }
            if (cursor_ >= json_.size()) return false;
            const char escape = json_[cursor_++];
            switch (escape) {
                case '"': case '\\': case '/': out->push_back(escape); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u':
                    if (json_.size() - cursor_ < 4) return false;
                    // Public plugin ids and setting keys are ASCII; escaped codepoints cannot
                    // match them unless the escape itself denotes an ASCII character.
                    for (int i = 0; i < 4; ++i) {
                        const char hex = json_[cursor_++];
                        uint32_t digit = 0;
                        if (hex >= '0' && hex <= '9') digit = static_cast<uint32_t>(hex - '0');
                        else if (hex >= 'a' && hex <= 'f') digit = static_cast<uint32_t>(hex - 'a' + 10);
                        else if (hex >= 'A' && hex <= 'F') digit = static_cast<uint32_t>(hex - 'A' + 10);
                        else return false;
                        if (i == 3) out->push_back(static_cast<char>(digit));
                    }
                    break;
                default: return false;
            }
        }
        return false;
    }

    bool literal(std::string_view value) {
        if (json_.substr(cursor_, value.size()) != value) return false;
        cursor_ += value.size();
        return true;
    }
    void whitespace() {
        while (cursor_ < json_.size() && (json_[cursor_] == ' ' || json_[cursor_] == '\n' ||
               json_[cursor_] == '\r' || json_[cursor_] == '\t')) ++cursor_;
    }
    bool consume(char value) {
        if (cursor_ >= json_.size() || json_[cursor_] != value) return false;
        ++cursor_;
        return true;
    }

    std::string_view json_;
    std::size_t cursor_ = 0;
};

QolResult read_registered_bool(QolOwnerToken owner, int index, int32_t* out_value) {
    if (index < 0 || static_cast<std::size_t>(index) >= kSettings.size() || out_value == nullptr)
        return fail(kQolErrInvalidArgument, "invalid settings config key");
    const auto* api = contracts::config_namespace_registered_if_available(&config_namespace_api());
    if (api == nullptr) return fail(kQolErrNotReady, "registered config API unavailable");
    constexpr uint32_t kMaxConfigProjectionBytes = 1024 * 1024;
    std::vector<char> buffer(kMaxConfigProjectionBytes);
    uint32_t size = 0;
    const QolResult read_result = api->read_registered(owner, buffer.data(),
                                                        static_cast<uint32_t>(buffer.size()), &size);
    if (!contracts::succeeded(read_result)) return read_result;
    if (size > buffer.size()) return fail(kQolErrInternal, "registered config size invalid");
    const BoolSetting& setting = kSettings[static_cast<std::size_t>(index)];
    bool found = false;
    bool wrong_type = false;
    if (!RegisteredBoolReader(std::string_view(buffer.data(), size)).read(
            setting.plugin_id, setting.key, out_value, &found, &wrong_type))
        return fail(kQolErrInternal, "registered config JSON invalid");
    if (!found) return fail(kQolErrNotFound, "registered config key not found");
    if (wrong_type)
        return fail(kQolErrInternal, "registered config bool has invalid type");
    return contracts::ok();
}

struct Request {
    SettingsConfigRequestStatus status{};
    QolOwnerToken owner = 0;
    int setting = 0;
    int32_t configured_before = 0;
    bool dispatched = false;
};

struct Bridge {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<Request, kSettingsConfigRequestCapacity> requests{};
    uint64_t next_id = 1;
    SettingsConfigDispatch dispatch = nullptr;
    void* context = nullptr;
    SettingsConfigApplyResultProvider apply_result_provider = nullptr;
    bool stopping = false;
    std::thread worker;
    ~Bridge() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
    }
};

Bridge& bridge() {
    static Bridge instance;
    return instance;
}

QolResult query_receipt(uint64_t id, SettingsConfigRequestStatus* out) {
    if (id == 0 || out == nullptr || out->size < sizeof(SettingsConfigRequestStatus))
        return fail(kQolErrInvalidArgument, "invalid request/status size");
    Bridge& state = bridge();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (const Request& request : state.requests) {
        if (request.status.request_id == id) {
            *out = request.status;
            return contracts::ok();
        }
    }
    return fail(id < state.next_id ? kQolErrCancelled : kQolErrNotFound,
                id < state.next_id ? "request receipt expired" : "unknown request");
}

QolResult read_bool_legacy(const char*, int32_t*) {
    return fail(kQolErrUnsupported, "owner token required for settings config read");
}

QolResult request_set_bool_legacy(const char*, int32_t, uint64_t* out_request) {
    if (out_request != nullptr) *out_request = 0;
    return fail(kQolErrUnsupported, "owner token required for settings config update");
}

QolResult query_request_legacy(uint64_t, SettingsConfigRequestStatus*) {
    return fail(kQolErrUnsupported, "owner token required for settings config receipt");
}

QolResult read_bool_for_owner(QolOwnerToken owner, const char* key, int32_t* out_value) {
    if (out_value == nullptr) return fail(kQolErrInvalidArgument, "null output");
    const int index = key_index(key);
    if (index < 0) return fail(kQolErrNotFound, "unknown public bool key");
    SettingsOwnerLease lease(owner);
    const QolResult authorized = check_settings_owner(lease);
    if (!contracts::succeeded(authorized)) return authorized;
    return read_registered_bool(owner, index, out_value);
}

QolResult request_set_bool_for_owner(QolOwnerToken owner, const char* key, int32_t desired,
                                     uint64_t* out_request) {
    if (out_request != nullptr) *out_request = 0;
    if (out_request == nullptr || !is_bool(desired))
        return fail(kQolErrInvalidArgument, "desired must be 0 or 1");
    const int index = key_index(key);
    if (index < 0) return fail(kQolErrNotFound, "unknown public bool key");
    SettingsOwnerLease lease(owner);
    const QolResult authorized = check_settings_owner(lease);
    if (!contracts::succeeded(authorized)) return authorized;
    int32_t configured = 0;
    const QolResult read_result = read_registered_bool(owner, index, &configured);
    if (!contracts::succeeded(read_result)) return read_result;

    Bridge& state = bridge();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.dispatch == nullptr || state.stopping)
            return fail(kQolErrNotReady, "native settings config worker not ready");
        Request* slot = nullptr;
        for (Request& request : state.requests) {
            if (request.status.request_id == 0) { slot = &request; break; }
            if (request.status.state != SettingsConfigRequestState::kQueued &&
                (slot == nullptr || request.status.request_id < slot->status.request_id))
                slot = &request;
        }
        if (slot == nullptr) return fail(kQolErrBlocked, "settings request capacity exhausted");
        *slot = Request{};
        slot->setting = index;
        slot->owner = owner;
        slot->configured_before = configured;
        slot->status.size = sizeof(SettingsConfigRequestStatus);
        slot->status.state = SettingsConfigRequestState::kQueued;
        slot->status.request_id = state.next_id++;
        slot->status.desired_value = desired;
        slot->status.configured_value = configured;
        *out_request = slot->status.request_id;
    }
    state.changed.notify_one();
    return contracts::ok();
}

QolResult query_request_for_owner(QolOwnerToken owner, uint64_t id,
                                  SettingsConfigRequestStatus* out_status) {
    SettingsOwnerLease lease(owner);
    const QolResult authorized = check_settings_owner(lease);
    if (!contracts::succeeded(authorized)) return authorized;
    return query_receipt(id, out_status);
}

QolResult native_config_dispatch(void*, QolOwnerToken owner, uint64_t id, const char* public_key,
                                 int32_t desired, int32_t configured_before) {
    const int index = key_index(public_key);
    if (index < 0) return fail(kQolErrNotFound, "unknown public bool key");
    const BoolSetting& setting = kSettings[static_cast<std::size_t>(index)];
    const auto* api = contracts::config_namespace_update_if_available(&config_namespace_api());
    if (api == nullptr) return fail(kQolErrNotReady, "config namespace update API unavailable");
    const char* value = desired != 0 ? "true" : "false";
    const QolResult updated = api->update_value(owner, setting.plugin_id, setting.key, value,
                                                 static_cast<uint32_t>(std::strlen(value)));
    const int32_t persisted = contracts::succeeded(updated) ? 1 : 0;
    const int32_t configured = persisted != 0 ? desired : configured_before;
    SettingsConfigApplyState apply = SettingsConfigApplyState::kNotSubmitted;
    int32_t error_code = updated.code;
    const char* error_text = contracts::succeeded(updated)
                                 ? ""
                                 : (updated.message != nullptr ? updated.message
                                                               : "configuration storage unavailable");
    const char* apply_message = persisted != 0 ? "configuration committed" : "not submitted";
    if (persisted != 0) {
        SettingsConfigApplyResultProvider provider = nullptr;
        {
            Bridge& state = bridge();
            std::lock_guard<std::mutex> lock(state.mutex);
            provider = state.apply_result_provider;
        }
        const CoreSettingsRuntimeApplyResult result = provider != nullptr
            ? provider()
            : CoreSettingsRuntimeApplyResult{setting.fallback_apply, 0,
                                             "runtime effect submitted"};
        apply = configured_before == desired ? SettingsConfigApplyState::kUnchanged : result.state;
        error_code = result.error_code;
        if (result.error_code != 0) error_text = result.message.c_str();
        apply_message = result.message.c_str();
    }
    return settings_config_complete(id, persisted, configured, apply, error_code,
                                    error_text, apply_message);
}

void dispatch_loop() {
    Bridge& state = bridge();
    std::unique_lock<std::mutex> lock(state.mutex);
    for (;;) {
        Request* next = nullptr;
        for (Request& request : state.requests) {
            if (request.status.request_id != 0 && !request.dispatched &&
                request.status.state == SettingsConfigRequestState::kQueued &&
                (next == nullptr || request.status.request_id < next->status.request_id)) next = &request;
        }
        if (state.stopping) return;
        if (next == nullptr) { state.changed.wait(lock); continue; }
        const Request snapshot = *next;
        const char* key = kSettings[static_cast<std::size_t>(snapshot.setting)].public_key;
        next->dispatched = true;
        const SettingsConfigDispatch dispatch = state.dispatch;
        void* context = state.context;
        lock.unlock();
        const QolResult result = dispatch(context, snapshot.owner, snapshot.status.request_id, key,
                                          snapshot.status.desired_value,
                                          snapshot.configured_before);
        if (!contracts::succeeded(result))
            (void)settings_config_complete(snapshot.status.request_id, 0,
                                           snapshot.configured_before,
                                           SettingsConfigApplyState::kNotSubmitted, result.code,
                                           result.message != nullptr ? result.message
                                                                     : "background config update failed",
                                           "not submitted");
        lock.lock();
    }
}

}  // namespace

const QolSettingsConfigApi& settings_config_api() {
    static const QolSettingsConfigApi api{sizeof(QolSettingsConfigApi), kSettingsConfigMajor,
        kSettingsConfigMinor, &read_bool_legacy, &request_set_bool_legacy, &query_request_legacy,
        &read_bool_for_owner, &request_set_bool_for_owner, &query_request_for_owner};
    return api;
}

const char* settings_config_key(uint32_t index) {
    return index < kSettings.size() ? kSettings[index].public_key : nullptr;
}

QolResult settings_config_install(SettingsConfigDispatch dispatch, void* context) {
    if (dispatch == nullptr) return fail(kQolErrInvalidArgument, "null dispatch");
    Bridge& state = bridge();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.dispatch != nullptr)
        return state.dispatch == dispatch && state.context == context ? ok() :
               fail(kQolErrConflict, "bridge already installed");
    state.stopping = false;
    state.dispatch = dispatch;
    state.context = context;
    state.worker = std::thread(&dispatch_loop);
    return ok();
}

QolResult settings_config_install_native() {
    if (contracts::config_namespace_update_if_available(&config_namespace_api()) == nullptr)
        return fail(kQolErrNotReady, "config namespace API unavailable");
    return settings_config_install(&native_config_dispatch, nullptr);
}

void settings_config_set_apply_result_provider(SettingsConfigApplyResultProvider provider) {
    Bridge& state = bridge();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.apply_result_provider = provider;
}

QolResult settings_config_publish(const int32_t*, uint32_t, uint64_t) {
    return fail(kQolErrUnsupported, "native Store replaces legacy configuration mirrors");
}

QolResult settings_config_complete(uint64_t id, int32_t persisted, int32_t configured,
                                   SettingsConfigApplyState apply, int32_t error_code,
                                   const char* error_text, const char* apply_message) {
    if (id == 0 || !is_bool(persisted) || !is_bool(configured) ||
        static_cast<uint32_t>(apply) > static_cast<uint32_t>(SettingsConfigApplyState::kUnchanged) ||
        error_text == nullptr || apply_message == nullptr)
        return fail(kQolErrInvalidArgument, "invalid receipt");
    Bridge& state = bridge();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (Request& request : state.requests) {
        if (request.status.request_id != id) continue;
        if (request.status.state != SettingsConfigRequestState::kQueued)
            return fail(kQolErrConflict, "request already completed");
        SettingsConfigRequestStatus& status = request.status;
        status.persisted = persisted;
        status.configured_value = configured;
        status.apply_state = apply;
        status.error_code = error_code;
        status.state = persisted != 0 && error_code == 0 && apply != SettingsConfigApplyState::kRejected
                           ? SettingsConfigRequestState::kSucceeded
                           : SettingsConfigRequestState::kFailed;
        std::strncpy(status.error, error_text, sizeof(status.error) - 1);
        status.error[sizeof(status.error) - 1] = '\0';
        std::strncpy(status.apply_message, apply_message, sizeof(status.apply_message) - 1);
        status.apply_message[sizeof(status.apply_message) - 1] = '\0';
        return ok();
    }
    return fail(kQolErrNotFound, "unknown request receipt");
}

void settings_config_shutdown() {
    Bridge& state = bridge();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stopping = true;
    }
    state.changed.notify_all();
    if (state.worker.joinable()) state.worker.join();
}

}  // namespace qol::host
