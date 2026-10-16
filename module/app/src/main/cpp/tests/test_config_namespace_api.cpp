#include "contracts/config_namespace.h"
#include "contracts/host_sdk.h"
#include "host/config_namespace.h"
#include "host/config_platform_bridge.h"
#include "host/core_settings_config_service.h"
#include "host/owner_identity.h"
#include "infra/log/qol_log.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct FakeBackend {
    std::map<std::string, std::map<std::string, std::string>> objects;
    std::mutex mutex;
    std::condition_variable condition;
    bool block_next_update = false;
    bool update_entered = false;
    bool release_update = false;
    int update_calls = 0;
};

QolResult error(int32_t code, const char* message) {
    return qol::contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

std::string serialize_object(const std::map<std::string, std::string>& object) {
    std::string output = "{";
    bool first = true;
    for (const auto& entry : object) {
        if (!first) output.push_back(',');
        first = false;
        output += "\"" + entry.first + "\":" + entry.second;
    }
    output.push_back('}');
    return output;
}

QolResult backend_read(void* context, const char* plugin_id, char* out, uint32_t capacity,
                       uint32_t* out_size) {
    if (plugin_id == nullptr || out_size == nullptr || (capacity != 0 && out == nullptr))
        return error(kQolErrInvalidArgument, "invalid read");
    auto& backend = *static_cast<FakeBackend*>(context);
    std::lock_guard<std::mutex> lock(backend.mutex);
    const auto found = backend.objects.find(plugin_id);
    if (found == backend.objects.end()) return error(kQolErrNotFound, "namespace not found");
    const std::string json = serialize_object(found->second);
    *out_size = static_cast<uint32_t>(json.size());
    if (capacity < json.size()) return error(kQolErrBlocked, "buffer too small");
    if (!json.empty()) std::memcpy(out, json.data(), json.size());
    return qol::contracts::ok();
}

QolResult backend_update(void* context, const char* caller_id, const char* target_id,
                         const char* key, const char* value, uint32_t size) {
    if (caller_id == nullptr || target_id == nullptr || key == nullptr || key[0] == '\0' ||
        (size != 0 && value == nullptr))
        return error(kQolErrInvalidArgument, "invalid update");
    const std::string json(value == nullptr ? "" : value, size);
    if (json.empty() || json == "{" || json == "not-json")
        return error(kQolErrInvalidArgument, "invalid JSON value");
    if (std::strcmp(key, "enable") == 0 && json != "true" && json != "false")
        return error(kQolErrInvalidArgument, "enable must be boolean");

    auto& backend = *static_cast<FakeBackend*>(context);
    std::unique_lock<std::mutex> lock(backend.mutex);
    ++backend.update_calls;
    if (backend.block_next_update) {
        backend.block_next_update = false;
        backend.update_entered = true;
        backend.condition.notify_all();
        if (!backend.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                return backend.release_update;
            })) return error(kQolErrTimeout, "update timed out");
    }
    const auto caller = backend.objects.find(caller_id);
    const auto target = backend.objects.find(target_id);
    if (caller == backend.objects.end()) return error(kQolErrBlocked, "caller not registered");
    if (target == backend.objects.end()) return error(kQolErrNotFound, "target not registered");
    const auto existing = target->second.find(key);
    if (std::strcmp(caller_id, target_id) != 0 && existing == target->second.end())
        return error(kQolErrNotFound, "cross-target key does not exist");
    target->second[key] = json;
    return qol::contracts::ok();
}

QolResult backend_read_registered(void*, char* out, uint32_t capacity, uint32_t* out_size) {
    static const std::string projection =
        "{\"core.settings\":{\"enable\":true},\"plugin.alpha\":{\"enable\":true}}";
    if (out_size == nullptr || (capacity != 0 && out == nullptr))
        return error(kQolErrInvalidArgument, "invalid registered read");
    *out_size = static_cast<uint32_t>(projection.size());
    if (capacity < projection.size()) return error(kQolErrBlocked, "buffer too small");
    std::memcpy(out, projection.data(), projection.size());
    return qol::contracts::ok();
}

QolResult backend_http_update(void*, const char*, const char*, uint32_t, int32_t*, char*,
                              uint32_t, uint32_t*) {
    return error(kQolErrUnsupported, "HTTP update unused in fake backend");
}

qol::contracts::QolOwnerToken register_owner(void* identity, const char* id) {
    assert(qol::host::owner_register(identity, id));
    qol::contracts::QolOwnerToken token = 0;
    uint64_t generation = 0;
    assert(qol::contracts::succeeded(
        qol::host::owner_identity_api().query(identity, &token, &generation)));
    return token;
}

const qol::features::core_settings::JsonValue* value_member(
    const qol::features::core_settings::JsonValue& object, const char* key) {
    for (const auto& entry : object.object) if (entry.first == key) return &entry.second;
    return nullptr;
}

}  // namespace

namespace qol::host {
void http_dispatch_set_op_enabled(bool) {}
}  // namespace qol::host

void qol_log_set_debug_enabled(bool) {}
void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {}
void frame_cache_start() {}
void frame_cache_stop() {}
namespace qol::infra {
QolResult commands_post_config_bool(const char*, bool, contracts::QolCommandTicket* out) {
    if (out != nullptr) *out = 1;
    return contracts::ok();
}
}  // namespace qol::infra

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace qol;
    host::config_namespace_reset_for_test();
    host::owner_identity_reset_for_test();
    const auto& api = host::config_namespace_api();
    assert(api.size == sizeof(contracts::QolConfigNamespaceApi));
    assert(api.major == contracts::kConfigNamespaceMajor && api.minor == 2);
    assert(api.read_json != nullptr && api.replace_json != nullptr && api.clear != nullptr);
    assert(api.update_value != nullptr && api.read_registered != nullptr);

    contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(contracts::QolHostSdk), contracts::kHostSdkMajor, 20};
    sdk.config_namespace = &api;
    assert(contracts::config_namespace_if_available(&sdk) == &api);
    assert(contracts::config_namespace_v11_if_available(&sdk) == nullptr);
    sdk.abi.minor = 21;
    assert(contracts::config_namespace_v11_if_available(&sdk) == &api);

    int identity_alpha = 1;
    int identity_beta = 2;
    int identity_settings = 3;
    int identity_stranger = 4;
    const auto alpha = register_owner(&identity_alpha, "plugin.alpha");
    const auto beta = register_owner(&identity_beta, "plugin.beta");
    const auto settings = register_owner(&identity_settings, "core.settings");
    const auto stranger = register_owner(&identity_stranger, "plugin.stranger");
    char buffer[2048]{};
    uint32_t size = 0;
    assert(api.read_json(alpha, buffer, sizeof(buffer), &size).code == kQolErrNotReady);

    FakeBackend backend;
    backend.objects["plugin.alpha"] = {{"enable", "true"}, {"existing", "1"}};
    backend.objects["plugin.beta"] = {{"enable", "false"}};
    backend.objects["core.settings"] = {{"enable", "true"}};
    assert(host::config_namespace_bind_backend(&backend, &backend_read, &backend_update,
                                               &backend_read_registered, &backend_http_update));
    assert(host::config_namespace_bind_backend(&backend, &backend_read, &backend_update,
                                               &backend_read_registered, &backend_http_update));
    FakeBackend conflicting_backend;
    assert(!host::config_namespace_bind_backend(&conflicting_backend, &backend_read,
                                                 &backend_update, &backend_read_registered,
                                                 &backend_http_update));

    assert(contracts::succeeded(api.read_json(alpha, buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size) == "{\"enable\":true,\"existing\":1}");
    assert(api.read_registered(alpha, buffer, sizeof(buffer), &size).code == kQolErrBlocked);
    assert(api.read_registered(stranger, buffer, sizeof(buffer), &size).code == kQolErrBlocked);
    assert(contracts::succeeded(api.read_registered(settings, buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size).find("plugin.stranger") == std::string::npos);
    assert(std::string(buffer, size).find("core.settings") < std::string(buffer, size).find("plugin.alpha"));

    const int updates_before_legacy = backend.update_calls;
    assert(api.replace_json(alpha, "{}", 2).code == kQolErrUnsupported);
    assert(api.clear(alpha).code == kQolErrUnsupported);
    assert(backend.update_calls == updates_before_legacy);

    assert(contracts::succeeded(api.update_value(alpha, "plugin.alpha", "own", "[1,true]", 8)));
    assert(contracts::succeeded(api.update_value(beta, "plugin.alpha", "existing", "2", 1)));
    assert(contracts::succeeded(api.update_value(beta, "plugin.alpha", "enable", "false", 5)));
    assert(api.update_value(beta, "plugin.alpha", "cross-new", "true", 4).code == kQolErrNotFound);
    assert(api.update_value(alpha, "plugin.alpha", "enable", "1", 1).code == kQolErrInvalidArgument);
    assert(api.update_value(alpha, "plugin.alpha", "bad", "not-json", 8).code == kQolErrInvalidArgument);
    assert(api.update_value(0xfeed, "plugin.alpha", "own", "false", 5).code == kQolErrBlocked);
    assert(api.update_value(beta, "not.registered", "own", "false", 5).code == kQolErrNotFound);

    {
        std::lock_guard<std::mutex> lock(backend.mutex);
        backend.block_next_update = true;
    }
    std::promise<QolResult> update_promise;
    auto update_future = update_promise.get_future();
    std::thread update_thread([&] {
        update_promise.set_value(api.update_value(alpha, "plugin.alpha", "inflight", "true", 4));
    });
    {
        std::unique_lock<std::mutex> lock(backend.mutex);
        assert(backend.condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return backend.update_entered;
        }));
    }
    std::promise<void> suspend_started_promise;
    std::promise<void> suspend_done_promise;
    auto suspend_started = suspend_started_promise.get_future();
    auto suspend_done = suspend_done_promise.get_future();
    std::thread suspend_thread([&] {
        suspend_started_promise.set_value();
        host::owner_suspend(&identity_alpha);
        suspend_done_promise.set_value();
    });
    assert(suspend_started.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    QolResult token_status = host::owner_verify_token(alpha);
    while (contracts::succeeded(token_status) && std::chrono::steady_clock::now() < deadline) {
        (void)suspend_done.wait_for(std::chrono::milliseconds(1));
        token_status = host::owner_verify_token(alpha);
    }
    assert(token_status.code == kQolErrBlocked);
    assert(suspend_done.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(api.read_json(alpha, buffer, sizeof(buffer), &size).code == kQolErrBlocked);
    assert(api.update_value(alpha, "plugin.alpha", "new", "true", 4).code == kQolErrBlocked);
    assert(!host::owner_resume(&identity_alpha));
    {
        std::lock_guard<std::mutex> lock(backend.mutex);
        backend.release_update = true;
    }
    backend.condition.notify_all();
    assert(update_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(contracts::succeeded(update_future.get()));
    assert(suspend_done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    update_thread.join();
    suspend_thread.join();
    assert(host::owner_resume(&identity_alpha));
    contracts::QolOwnerToken resumed_alpha = 0;
    uint64_t generation = 0;
    assert(contracts::succeeded(host::owner_identity_api().query(
        &identity_alpha, &resumed_alpha, &generation)));
    assert(resumed_alpha != 0 && resumed_alpha != alpha);
    assert(contracts::succeeded(api.read_json(resumed_alpha, buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size).find("\"inflight\":true") != std::string::npos);

    host::config_namespace_reset_for_test();
    host::owner_identity_reset_for_test();

    std::string root = std::string(argv[1]) + "/config-namespace-store-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    const std::string config_dir = root + "/settings";
    assert(::mkdir(config_dir.c_str(), 0700) == 0);
    {
        std::ofstream output(config_dir + "/config.json", std::ios::binary);
        output << "{\"plugin.beta\":{\"enable\":false,\"existing\":{\"x\":1}},"
                  "\"future.unregistered\":{\"keep\":true}}";
        assert(output.good());
    }
    assert(host::config_store_bootstrap(config_dir) == 0);
    const std::vector<std::string> registered_ids = {"core.settings", "plugin.alpha", "plugin.beta"};
    assert(host::core_settings_config_service()
               .seed_registered_plugins(registered_ids).result.ok());
    int real_alpha_identity = 11;
    int real_settings_identity = 13;
    int real_unregistered_identity = 14;
    const auto real_alpha = register_owner(&real_alpha_identity, "plugin.alpha");
    const auto real_settings = register_owner(&real_settings_identity, "core.settings");
    const auto real_unregistered = register_owner(&real_unregistered_identity, "plugin.stranger");
    assert(contracts::succeeded(api.update_value(real_alpha, "plugin.alpha", "own", "[1,2]", 5)));
    assert(contracts::succeeded(api.update_value(real_alpha, "plugin.beta", "existing",
                                                "{\"x\":2}", 7)));
    assert(api.update_value(real_alpha, "plugin.beta", "missing", "true", 4).code == kQolErrNotFound);
    assert(contracts::succeeded(api.update_value(real_alpha, "plugin.beta", "enable", "true", 4)));
    assert(api.read_json(real_unregistered, buffer, sizeof(buffer), &size).code == kQolErrInvalidArgument);
    assert(api.read_registered(real_unregistered, buffer, sizeof(buffer), &size).code == kQolErrBlocked);
    assert(contracts::succeeded(api.read_registered(real_settings, buffer, sizeof(buffer), &size)));
    const std::string registered_projection(buffer, size);
    assert(registered_projection.find("plugin.alpha") < registered_projection.find("plugin.beta"));
    assert(registered_projection.find("future.unregistered") == std::string::npos);
    assert(registered_projection.find("plugin.stranger") == std::string::npos);
    qol::features::core_settings::JsonValue beta_namespace;
    assert(host::core_settings_config_service().read_namespace("plugin.beta", &beta_namespace).ok());
    const auto* beta_enable = value_member(beta_namespace, "enable");
    assert(beta_enable != nullptr && beta_enable->kind ==
           qol::features::core_settings::JsonValue::Kind::kBoolean && beta_enable->boolean);
    const auto* existing_value = value_member(beta_namespace, "existing");
    assert(existing_value != nullptr && existing_value->kind ==
           qol::features::core_settings::JsonValue::Kind::kObject &&
           value_member(*existing_value, "x") != nullptr);

    const std::string http_body =
        "{\"plugin_id\":\"plugin.beta\",\"key\":\"existing\",\"value\":{\"x\":3}}";
    int32_t http_status = 0;
    uint32_t http_size = 0;
    assert(contracts::succeeded(api.update_http_request(
        real_settings, http_body.data(), static_cast<uint32_t>(http_body.size()), &http_status,
        buffer, sizeof(buffer), &http_size)));
    assert(http_status == 200);
    assert(std::string(buffer, http_size) ==
           "{\"ok\":true,\"plugin_id\":\"plugin.beta\",\"key\":\"existing\",\"value\":{\"x\":3}}");
    const std::string missing_http_key =
        "{\"plugin_id\":\"plugin.beta\",\"key\":\"not-created\",\"value\":true}";
    assert(contracts::succeeded(api.update_http_request(
        real_settings, missing_http_key.data(), static_cast<uint32_t>(missing_http_key.size()),
        &http_status, buffer, sizeof(buffer), &http_size)));
    assert(http_status == 404);
    assert(contracts::succeeded(api.update_http_request(real_settings, "{bad", 4,
        &http_status, buffer, sizeof(buffer), &http_size)));
    assert(http_status == 400);

    const std::string store_path = config_dir + "/config.json";
    const uint64_t revision_before_storage_failure = host::core_settings_config_service().revision();
    assert(::unlink(store_path.c_str()) == 0);
    assert(::mkdir(store_path.c_str(), 0700) == 0);
    const std::string failed_http_update =
        "{\"plugin_id\":\"plugin.beta\",\"key\":\"existing\",\"value\":{\"x\":4}}";
    assert(contracts::succeeded(api.update_http_request(
        real_settings, failed_http_update.data(), static_cast<uint32_t>(failed_http_update.size()),
        &http_status, buffer, sizeof(buffer), &http_size)));
    assert(http_status == 500 && std::string(buffer, http_size).find("config save failed") !=
                                   std::string::npos);
    assert(host::core_settings_config_service().revision() == revision_before_storage_failure);
    assert(host::core_settings_config_service().read_namespace("plugin.beta", &beta_namespace).ok());
    existing_value = value_member(beta_namespace, "existing");
    assert(existing_value != nullptr && value_member(*existing_value, "x")->scalar == "3");
    std::puts("config_namespace_api_tests: pass");
}
