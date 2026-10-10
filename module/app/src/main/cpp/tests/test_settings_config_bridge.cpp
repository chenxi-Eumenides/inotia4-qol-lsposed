#include "host/config_namespace.h"
#include "host/core_settings_config_service.h"
#include "host/owner_identity.h"
#include "host/settings_config_bridge.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace contracts = qol::contracts;

namespace {

using namespace qol::contracts;
using namespace qol::features::core_settings;
using namespace qol::host;

constexpr QolOwnerToken kSettingsOwner = 71;
constexpr QolOwnerToken kOtherOwner = 72;
constexpr uint32_t kFieldCount = kSettingsConfigBoolCount;
std::array<int32_t, kFieldCount> g_values{0, 0, 0, 1, 0, 0, 1, 0, 0, 0};
std::atomic<bool> g_fail_persist{false};
std::atomic<bool> g_block_update{false};
std::atomic<bool> g_update_entered{false};
std::atomic<bool> g_release_update{false};
std::atomic<bool> g_owner_suspended{false};
std::atomic<uint32_t> g_namespace_update_calls{0};
std::mutex g_owner_mutex;
std::condition_variable g_owner_changed;
uint32_t g_owner_calls = 0;
int32_t g_apply_error = 0;
SettingsConfigApplyState g_apply_state = SettingsConfigApplyState::kUnchanged;

constexpr const char* kKeys[kFieldCount] = {
    "stackLimitIncrease", "moveMergeEnabled", "opEnabled", "extensionBagEnabled",
    "customRecipeEnabled", "autoSellEnabled", "apiEnabled", "debugLogEnabled",
    "simpleModeEnabled", "pushAllEnabled"};
constexpr const char* kPlugins[kFieldCount] = {
    "stack_limit", "move_merge", "core.settings", "extension_bag", "custom_recipe",
    "autosell", "core.settings", "core.settings", "simple_mode", "push_all"};
constexpr const char* kFields[kFieldCount] = {
    "enable", "enable", "opEnabled", "enable", "enable", "enable", "apiEnabled",
    "debugLogEnabled", "enable", "enable"};

QolResult test_error(int32_t code, const char* message) {
    return error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

int key_index(const char* key) {
    if (key == nullptr) return -1;
    for (uint32_t i = 0; i < kFieldCount; ++i)
        if (std::strcmp(key, kKeys[i]) == 0) return static_cast<int>(i);
    return -1;
}

class TestOwnerLease {
public:
    explicit TestOwnerLease(QolOwnerToken token) : token_(token) {
        std::lock_guard<std::mutex> lock(g_owner_mutex);
        valid_ = token_ == kSettingsOwner && !g_owner_suspended.load();
        if (valid_) ++g_owner_calls;
    }
    ~TestOwnerLease() {
        if (!valid_) return;
        std::lock_guard<std::mutex> lock(g_owner_mutex);
        --g_owner_calls;
        g_owner_changed.notify_all();
    }
    bool valid() const { return valid_; }

private:
    QolOwnerToken token_;
    bool valid_ = false;
};

QolResult fake_namespace_read(QolOwnerToken owner, char* out, uint32_t capacity,
                              uint32_t* out_size) {
    TestOwnerLease lease(owner);
    if (!lease.valid()) return test_error(kQolErrBlocked, "owner unavailable");
    static constexpr char own_config[] = "{\"enable\":true}";
    if (out == nullptr || out_size == nullptr || capacity < sizeof(own_config) - 1)
        return test_error(kQolErrBlocked, "buffer too small");
    std::memcpy(out, own_config, sizeof(own_config) - 1);
    *out_size = sizeof(own_config) - 1;
    return ok();
}

QolResult fake_namespace_replace(QolOwnerToken, const char*, uint32_t) {
    return test_error(kQolErrUnsupported, "replace unsupported");
}

QolResult fake_namespace_clear(QolOwnerToken) {
    return test_error(kQolErrUnsupported, "clear unsupported");
}

QolResult fake_namespace_update(QolOwnerToken owner, const char* target, const char* key,
                                const char* json, uint32_t size) {
    g_namespace_update_calls.fetch_add(1);
    TestOwnerLease lease(owner);
    if (!lease.valid()) return test_error(kQolErrBlocked, "owner unavailable");
    if (target == nullptr || key == nullptr || json == nullptr)
        return test_error(kQolErrInvalidArgument, "invalid update");
    int setting = -1;
    for (uint32_t i = 0; i < kFieldCount; ++i)
        if (std::strcmp(target, kPlugins[i]) == 0 && std::strcmp(key, kFields[i]) == 0) {
            setting = static_cast<int>(i);
            break;
        }
    if (setting < 0) return test_error(kQolErrNotFound, "missing setting");
    const std::string value(json, size);
    if (value != "true" && value != "false")
        return test_error(kQolErrInvalidArgument, "invalid bool JSON");
    if (g_block_update.load()) {
        g_update_entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!g_release_update.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!g_release_update.load()) return test_error(kQolErrTimeout, "update timeout");
    }
    if (g_fail_persist.load()) return test_error(kQolErrInternal, "configuration storage unavailable");
    g_values[static_cast<std::size_t>(setting)] = value == "true" ? 1 : 0;
    if (g_apply_error != 0) {
        g_apply_state = SettingsConfigApplyState::kRejected;
    } else if (setting == 6) {
        g_apply_state = SettingsConfigApplyState::kPlatformPending;
    } else if (setting == 2 || setting == 7) {
        g_apply_state = SettingsConfigApplyState::kApplied;
    } else {
        g_apply_state = SettingsConfigApplyState::kPendingFrame;
    }
    return ok();
}

QolResult fake_namespace_read_registered(QolOwnerToken owner, char* out, uint32_t capacity,
                                         uint32_t* out_size) {
    TestOwnerLease lease(owner);
    if (!lease.valid()) return test_error(kQolErrBlocked, "owner unavailable");
    if (out == nullptr || out_size == nullptr)
        return test_error(kQolErrInvalidArgument, "invalid read buffer");
    const auto boolean = [](int32_t value) { return value == 0 ? "false" : "true"; };
    std::string projection = "{\"autosell\":{\"enable\":" +
        std::string(boolean(g_values[5])) + "},\"core.settings\":{\"enable\":true,\"opEnabled\":" +
        boolean(g_values[2]) + ",\"apiEnabled\":" + boolean(g_values[6]) +
        ",\"debugLogEnabled\":" + boolean(g_values[7]) +
        "},\"custom_recipe\":{\"enable\":" + boolean(g_values[4]) +
        "},\"extension_bag\":{\"enable\":" + boolean(g_values[3]) +
        "},\"move_merge\":{\"enable\":" + boolean(g_values[1]) +
        "},\"push_all\":{\"enable\":" + boolean(g_values[9]) +
        "},\"simple_mode\":{\"enable\":" + boolean(g_values[8]) +
        "},\"stack_limit\":{\"enable\":" + boolean(g_values[0]) + "}}";
    *out_size = static_cast<uint32_t>(projection.size());
    if (capacity < projection.size()) return test_error(kQolErrBlocked, "buffer too small");
    std::memcpy(out, projection.data(), projection.size());
    return ok();
}

const QolConfigNamespaceApi kNamespaceApi{
    sizeof(QolConfigNamespaceApi), kConfigNamespaceMajor, kConfigNamespaceMinor,
    &fake_namespace_read, &fake_namespace_replace, &fake_namespace_clear,
    &fake_namespace_update, &fake_namespace_read_registered, nullptr};

}  // namespace

namespace qol::host {

QolResult owner_namespace_call_begin(contracts::QolOwnerToken owner, OwnerNamespaceCall* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, "null owner", 10);
    std::lock_guard<std::mutex> lock(g_owner_mutex);
    if (g_owner_suspended.load() || owner != kSettingsOwner)
        return contracts::error(kQolErrBlocked, "owner unavailable", 17);
    out->identity = reinterpret_cast<void*>(static_cast<uintptr_t>(owner));
    out->plugin_id = "core.settings";
    out->active = true;
    ++g_owner_calls;
    return contracts::ok();
}

void owner_namespace_call_end(OwnerNamespaceCall* call) {
    if (call == nullptr || !call->active) return;
    std::lock_guard<std::mutex> lock(g_owner_mutex);
    --g_owner_calls;
    call->active = false;
    g_owner_changed.notify_all();
}

const contracts::QolConfigNamespaceApi& config_namespace_api() { return kNamespaceApi; }

CoreSettingsRuntimeApplyResult fake_apply_result() {
    return {g_apply_state, g_apply_error,
            g_apply_error == 0 ? "fake main-thread/runtime apply" : "fake gate queue rejected"};
}

}  // namespace qol::host

namespace qol::features::core_settings {

ConfigStoreResult parse_json_document(const std::string&, JsonValue* out) {
    if (out == nullptr) return {ConfigStoreError::kInvalidArgument, "null JSON result", false};
    out->kind = JsonValue::Kind::kObject;
    for (uint32_t i = 0; i < kFieldCount; ++i) {
        JsonValue* object = nullptr;
        for (auto& entry : out->object)
            if (entry.first == kPlugins[i]) object = &entry.second;
        if (object == nullptr) {
            out->object.emplace_back(kPlugins[i], JsonValue{});
            object = &out->object.back().second;
            object->kind = JsonValue::Kind::kObject;
            JsonValue enabled;
            enabled.kind = JsonValue::Kind::kBoolean;
            enabled.boolean = g_values[i] != 0;
            object->object.emplace_back("enable", enabled);
        }
        if (std::strcmp(kPlugins[i], "core.settings") == 0 && i >= 2) {
            JsonValue value;
            value.kind = JsonValue::Kind::kBoolean;
            value.boolean = g_values[i] != 0;
            object->object.emplace_back(kFields[i], value);
        }
    }
    return {};
}

}  // namespace qol::features::core_settings

namespace {

using namespace qol::contracts;
using namespace qol::host;

bool wait_for(const std::function<bool()>& predicate, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool request_status(uint64_t id, SettingsConfigRequestStatus* out) {
    out->size = sizeof(*out);
    return contracts::succeeded(settings_config_api().query_request_for_owner(kSettingsOwner, id, out));
}

void test_owner_api_and_receipts() {
    const auto& api = settings_config_api();
    assert(api.size == sizeof(QolSettingsConfigApi));
    assert(api.major == kSettingsConfigMajor && api.minor == kSettingsConfigMinor);
    assert(api.read_bool != nullptr && api.request_set_bool != nullptr && api.query_request != nullptr);
    assert(api.read_bool_for_owner != nullptr && api.request_set_bool_for_owner != nullptr &&
           api.query_request_for_owner != nullptr);
    int32_t value = -1;
    uint64_t id = 9;
    SettingsConfigRequestStatus status{};
    status.size = sizeof(status);
    assert(api.read_bool("apiEnabled", &value).code == kQolErrUnsupported);
    assert(api.request_set_bool("apiEnabled", 1, &id).code == kQolErrUnsupported && id == 0);
    assert(api.query_request(1, &status).code == kQolErrUnsupported);
    assert(api.read_bool_for_owner(kOtherOwner, "apiEnabled", &value).code == kQolErrBlocked);
    assert(api.request_set_bool_for_owner(kOtherOwner, "apiEnabled", 1, &id).code == kQolErrBlocked);
    assert(api.read_bool_for_owner(999, "apiEnabled", &value).code == kQolErrBlocked);
    g_owner_suspended = true;
    assert(api.read_bool_for_owner(kSettingsOwner, "apiEnabled", &value).code == kQolErrBlocked);
    g_owner_suspended = false;

    settings_config_set_apply_result_provider(&fake_apply_result);
    assert(contracts::succeeded(settings_config_install_native()));
    assert(contracts::succeeded(api.read_bool_for_owner(kSettingsOwner, "apiEnabled", &value)) &&
           value == 1);
    // apiEnabled=false does not disable the resident settings UI writer.
    g_values[6] = 0;
    assert(contracts::succeeded(api.request_set_bool_for_owner(kSettingsOwner, "simpleModeEnabled", 1, &id)));
    assert(wait_for([&] { return request_status(id, &status) &&
                                status.state == SettingsConfigRequestState::kSucceeded; }));
    assert(status.persisted == 1 && status.configured_value == 1 &&
           status.apply_state == SettingsConfigApplyState::kPendingFrame);

    // An already leased transaction drains during owner suspend; the queued next request
    // revalidates its token at dispatch and fails after the owner is suspended.
    g_block_update = true;
    g_release_update = false;
    g_update_entered = false;
    g_namespace_update_calls = 0;
    uint64_t leased_request = 0;
    uint64_t queued_request = 0;
    assert(contracts::succeeded(api.request_set_bool_for_owner(
        kSettingsOwner, "stackLimitIncrease", 0, &leased_request)));
    assert(wait_for([&] { return g_update_entered.load(); }));
    assert(contracts::succeeded(api.request_set_bool_for_owner(
        kSettingsOwner, "moveMergeEnabled", 1, &queued_request)));
    std::promise<void> suspend_done_promise;
    auto suspend_done = suspend_done_promise.get_future();
    std::thread suspend_thread([&] {
        {
            std::unique_lock<std::mutex> lock(g_owner_mutex);
            g_owner_suspended = true;
            g_owner_changed.wait(lock, [] { return g_owner_calls == 0; });
        }
        suspend_done_promise.set_value();
    });
    assert(wait_for([&] { return g_owner_suspended.load(); }));
    g_release_update = true;
    assert(suspend_done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(wait_for([&] { return g_namespace_update_calls.load() >= 2; }));
    suspend_thread.join();
    g_owner_suspended = false;
    assert(wait_for([&] { return request_status(leased_request, &status) &&
                                status.state == SettingsConfigRequestState::kSucceeded; }));
    assert(status.persisted == 1);
    assert(wait_for([&] { return request_status(queued_request, &status) &&
                                status.state == SettingsConfigRequestState::kFailed; }));
    assert(status.persisted == 0 && status.configured_value == 0 &&
           status.apply_state == SettingsConfigApplyState::kNotSubmitted);

    assert(contracts::succeeded(api.request_set_bool_for_owner(kSettingsOwner, "opEnabled", 1, &id)));
    assert(wait_for([&] { return request_status(id, &status) &&
                                status.state == SettingsConfigRequestState::kSucceeded; }));
    assert(status.apply_state == SettingsConfigApplyState::kApplied);

    g_fail_persist = true;
    const int32_t old_value = g_values[0];
    assert(contracts::succeeded(api.request_set_bool_for_owner(kSettingsOwner, "stackLimitIncrease", 1, &id)));
    assert(wait_for([&] { return request_status(id, &status) &&
                                status.state == SettingsConfigRequestState::kFailed; }));
    assert(status.persisted == 0 && status.configured_value == old_value &&
           status.apply_state == SettingsConfigApplyState::kNotSubmitted);
    g_fail_persist = false;
    assert(contracts::succeeded(api.request_set_bool_for_owner(kSettingsOwner, "stackLimitIncrease", 1, &id)));
    assert(wait_for([&] { return request_status(id, &status) &&
                                status.state == SettingsConfigRequestState::kSucceeded; }));
    assert(status.persisted == 1 && status.configured_value == 1);

    g_apply_error = kQolErrBlocked;
    assert(contracts::succeeded(api.request_set_bool_for_owner(kSettingsOwner, "moveMergeEnabled", 1, &id)));
    assert(wait_for([&] { return request_status(id, &status) &&
                                status.state == SettingsConfigRequestState::kFailed; }));
    assert(status.persisted == 1 && status.configured_value == 1 &&
           status.apply_state == SettingsConfigApplyState::kRejected &&
           status.error_code == kQolErrBlocked);
    g_apply_error = 0;
    settings_config_shutdown();
    assert(api.request_set_bool_for_owner(kSettingsOwner, "stackLimitIncrease", 0, &id).code ==
           kQolErrNotReady);
}

}  // namespace

int main() {
    test_owner_api_and_receipts();
    std::puts("settings_config_bridge_tests: PASS (owner-token UI adapter and request receipts)");
}
