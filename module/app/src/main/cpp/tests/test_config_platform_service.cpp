#include "contracts/host_sdk.h"
#include "host/config_namespace.h"
#include "host/config_platform_bridge.h"
#include "host/core_settings_config_service.h"
#include "host/owner_identity.h"
#include "host/settings_config_bridge.h"
#include "core/native/game_cache.h"
#include "infra/config.h"
#include "infra/commands.h"
#include "infra/log/qol_log.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using qol::features::core_settings::ModuleConfigPatch;

struct Observation {
    uint64_t revision = 0;
    int port = 0;
};

struct ObserverState {
    std::vector<Observation> values;
};

void observe(void* context, uint64_t revision,
             const qol::features::core_settings::ModuleConfig& config) {
    auto& state = *static_cast<ObserverState*>(context);
    state.values.push_back({revision, config.listen_port});
}

int g_op_enabled = -1;
bool g_debug_enabled = false;
int g_frame_cache_starts = 0;
int g_frame_cache_stops = 0;
std::mutex g_effect_mutex;
std::condition_variable g_effect_changed;
bool g_pause_move_merge = false;
bool g_move_merge_entered = false;
bool g_release_move_merge = false;
std::vector<std::string> g_effect_order;

struct OrderedObservations {
    std::mutex mutex;
    std::vector<uint64_t> revisions;
};

struct ReentrantObserverState {
    uint64_t trigger_revision = 0;
    bool triggered = false;
    qol::host::CoreSettingsConfigCommit nested_commit;
    std::mutex mutex;
    std::vector<uint64_t> revisions;
};

void observe_ordered(void* context, uint64_t revision,
                     const qol::features::core_settings::ModuleConfig&) {
    auto& state = *static_cast<OrderedObservations*>(context);
    qol::features::core_settings::ModuleConfig snapshot;
    assert(qol::host::core_settings_config_service().snapshot(&snapshot).ok());
    std::lock_guard<std::mutex> lock(state.mutex);
    state.revisions.push_back(revision);
}

void observe_reentrant(void* context, uint64_t revision,
                       const qol::features::core_settings::ModuleConfig&) {
    auto& state = *static_cast<ReentrantObserverState*>(context);
    qol::features::core_settings::ModuleConfig snapshot;
    assert(qol::host::core_settings_config_service().snapshot(&snapshot).ok());
    bool commit_nested = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.revisions.push_back(revision);
        if (!state.triggered && revision == state.trigger_revision) {
            state.triggered = true;
            commit_nested = true;
        }
    }
    if (commit_nested) {
        ModuleConfigPatch patch;
        patch.debug_log_enabled = true;
        state.nested_commit = qol::host::core_settings_config_service().commit(patch);
    }
}

bool wait_until(const std::function<bool()>& predicate) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}  // namespace

namespace qol::host {
void http_dispatch_set_op_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_effect_mutex);
    g_op_enabled = enabled ? 1 : 0;
    g_effect_order.push_back(enabled ? "op=1" : "op=0");
}
}  // namespace qol::host

namespace qol::infra {
QolResult commands_post_config_bool(const char* key, bool enabled,
                                    contracts::QolCommandTicket* out) {
    if (out != nullptr) *out = 1;
    std::unique_lock<std::mutex> lock(g_effect_mutex);
    if (key != nullptr && std::strcmp(key, "moveMergeEnabled") == 0) {
        g_effect_order.push_back(enabled ? "moveMergeEnabled=1" : "moveMergeEnabled=0");
        if (g_pause_move_merge) {
            g_move_merge_entered = true;
            g_effect_changed.notify_all();
            g_effect_changed.wait(lock, [] { return g_release_move_merge; });
        }
    }
    return contracts::ok();
}
}  // namespace qol::infra

void qol_log_set_debug_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_effect_mutex);
    g_debug_enabled = enabled;
    g_effect_order.push_back(enabled ? "debug=1" : "debug=0");
}
void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {}
void frame_cache_start() { ++g_frame_cache_starts; }
void frame_cache_stop() { ++g_frame_cache_stops; }

namespace {

qol::contracts::QolOwnerToken register_owner(void* identity, const char* plugin_id) {
    assert(qol::host::owner_register(identity, plugin_id));
    qol::contracts::QolOwnerToken token = 0;
    uint64_t generation = 0;
    assert(qol::contracts::succeeded(
        qol::host::owner_identity_api().query(identity, &token, &generation)));
    return token;
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string root = std::string(argv[1]) + "/config-platform-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    const std::string directory = root + "/settings";
    assert(::mkdir(directory.c_str(), 0700) == 0);

    auto& service = qol::host::core_settings_config_service();
    qol::features::core_settings::ModuleConfig config;
    assert(service.state() == qol::host::CoreSettingsConfigServiceState::kUninitialized);
    assert(!qol::host::config_store_bootstrapped());
    assert(qol::host::config_store_bootstrap(directory) == 0);
    assert(service.state() == qol::host::CoreSettingsConfigServiceState::kReady);
    assert(qol::host::config_store_bootstrapped());
    assert(service.revision() == 1);
    assert(qol::host::config_store_bootstrap(directory) == 0 && service.revision() == 1);
    assert(qol::host::config_store_bootstrap(root + "/other") == -1);
    assert(service.revision() == 1);
    const auto seeded = service.seed_registered_plugins({
        "core.settings", "sample.plugin", "stack_limit", "move_merge", "extension_bag",
        "custom_recipe", "simple_mode", "push_all", "autosell"});
    assert(seeded.result.ok() && seeded.durable_commit && service.revision() == 2);

    ObserverState observations;
    assert(service.subscribe_platform(&observe, &observations).ok());
    assert(observations.values.size() == 1);
    assert(observations.values[0].revision == 2 && observations.values[0].port == 8088);

    ReentrantObserverState reentrant_observations;
    reentrant_observations.trigger_revision = service.revision() + 1;
    assert(service.subscribe_platform(&observe_reentrant, &reentrant_observations).ok());
    {
        std::lock_guard<std::mutex> lock(g_effect_mutex);
        g_effect_order.clear();
    }
    ModuleConfigPatch outer_patch;
    outer_patch.op_enabled = true;
    std::promise<qol::host::CoreSettingsConfigCommit> outer_promise;
    auto outer_future = outer_promise.get_future();
    std::thread outer_thread([&] { outer_promise.set_value(service.commit(outer_patch)); });
    if (outer_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::fprintf(stderr, "reentrant observer commit did not complete\n");
        std::_Exit(2);
    }
    outer_thread.join();
    const auto outer_commit = outer_future.get();
    assert(outer_commit.committed && outer_commit.result.persisted &&
           outer_commit.apply_state == qol::contracts::SettingsConfigApplyState::kApplied);
    assert(reentrant_observations.nested_commit.committed &&
           reentrant_observations.nested_commit.result.persisted &&
           reentrant_observations.nested_commit.apply_state ==
               qol::contracts::SettingsConfigApplyState::kPendingFrame &&
           reentrant_observations.nested_commit.apply_error_code == 0 &&
           reentrant_observations.nested_commit.apply_message.find("queued") != std::string::npos);
    assert(outer_commit.revision == 3 && reentrant_observations.nested_commit.revision == 4 &&
           service.revision() == 4);
    assert(service.snapshot(&config).ok() && config.op_enabled && config.debug_log_enabled);
    {
        std::lock_guard<std::mutex> lock(g_effect_mutex);
        assert(g_effect_order.size() == 2 && g_effect_order[0] == "op=1" &&
               g_effect_order[1] == "debug=1");
        assert(g_op_enabled == 1 && g_debug_enabled);
    }
    assert(observations.values.size() == 3);
    assert(observations.values[1].revision == 3 && observations.values[2].revision == 4);
    {
        std::lock_guard<std::mutex> lock(reentrant_observations.mutex);
        assert(reentrant_observations.revisions.size() == 3);
        assert(reentrant_observations.revisions[0] == 2 &&
               reentrant_observations.revisions[1] == 3 &&
               reentrant_observations.revisions[2] == 4);
    }

    auto& namespaces = qol::host::config_namespace_api();
    assert(qol::host::config_platform_bind_namespace_store());
    int identity = 0;
    const auto owner = register_owner(&identity, "sample.plugin");
    int settings_identity = 0;
    const auto settings_owner = register_owner(&settings_identity, "core.settings");
    const auto& settings_api = qol::host::settings_config_api();
    int32_t current_stack_limit = -1;
    assert(qol::contracts::succeeded(settings_api.read_bool_for_owner(
        settings_owner, "stackLimitIncrease", &current_stack_limit)));
    assert(current_stack_limit == 0);
    uint64_t settings_request = 0;
    assert(qol::contracts::succeeded(settings_api.request_set_bool_for_owner(
        settings_owner, "stackLimitIncrease", 1, &settings_request)));
    qol::contracts::SettingsConfigRequestStatus settings_status{};
    settings_status.size = sizeof(settings_status);
    for (int attempt = 0; attempt < 500; ++attempt) {
        settings_status.size = sizeof(settings_status);
        assert(qol::contracts::succeeded(settings_api.query_request_for_owner(
            settings_owner, settings_request, &settings_status)));
        if (settings_status.state != qol::contracts::SettingsConfigRequestState::kQueued) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(settings_status.state == qol::contracts::SettingsConfigRequestState::kSucceeded);
    assert(settings_status.persisted == 1 && settings_status.configured_value == 1 &&
           settings_status.apply_state == qol::contracts::SettingsConfigApplyState::kPendingFrame);
    assert(qol::contracts::succeeded(namespaces.update_value(owner, "sample.plugin", "value",
                                                               "7", 1)));
    qol::features::core_settings::JsonValue stored_namespace;
    assert(service.read_namespace("sample.plugin", &stored_namespace).ok());
    char namespace_json[128]{};
    uint32_t namespace_size = 0;
    assert(qol::contracts::succeeded(namespaces.read_json(owner, namespace_json,
                                                         sizeof(namespace_json), &namespace_size)));
    assert(std::string(namespace_json, namespace_size).find("\"value\":7") != std::string::npos);

    config.listen_address = "127.0.0.1";
    config.listen_port = 8123;
    config.stack_limit_increase = true;
    config.simple_mode_enabled = true;
    config.push_all_enabled = true;
    config.auto_sell_enabled = true;
    config.move_merge_enabled = true;
    config.extension_bag_enabled = false;
    config.custom_recipe_enabled = true;
    config.op_enabled = true;
    config.debug_log_enabled = true;
    qol::host::config_seed_runtime_config(config);
    qol::host::config_apply_safe_native_state(config);
    qol::host::config_apply_api_enabled(true);
    qol::host::config_apply_api_enabled(false);
    assert(qol::infra::config_get_bool("plugin.stack_limit.enabled", false));
    assert(qol::infra::config_get_bool("plugin.simple_mode.enabled", false));
    assert(qol::infra::config_get_bool("plugin.push_all.enabled", false));
    assert(qol::infra::config_get_bool("plugin.autosell.enabled", false));
    assert(qol::infra::config_get_bool("moveMergeEnabled", false));
    assert(!qol::infra::config_get_bool("extensionBagEnabled", true));
    assert(qol::infra::config_get_bool("customRecipeEnabled", false));
    assert(qol::infra::config_get_string("listenAddress", "") == std::string("127.0.0.1"));
    assert(qol::infra::config_get_int("listenPort", 0) == 8123);
    assert(g_op_enabled == 1 && g_debug_enabled);
    assert(g_frame_cache_starts == 1 && g_frame_cache_stops == 1);

    ModuleConfigPatch pre_fallback_listen_patch;
    pre_fallback_listen_patch.listen_port = 9001;
    assert(service.commit(pre_fallback_listen_patch).committed);
    const uint64_t before_stale_report = service.revision();
    assert(!service.report_bind_failure(before_stale_report - 1, 9001));
    assert(!service.report_bind_failure(before_stale_report, 8088));
    assert(service.report_bind_failure(before_stale_report, 9001));
    assert(service.revision() == before_stale_report + 1);
    assert(!service.report_bind_failure(before_stale_report, 9001));
    assert(!service.report_bind_failure(service.revision(), 8088));
    assert(service.snapshot(&config).ok() && config.listen_port == 8088);

    OrderedObservations ordered_observations;
    assert(service.subscribe_platform(&observe_ordered, &ordered_observations).ok());
    ModuleConfigPatch listen_patch;
    listen_patch.listen_port = 9001;
    const auto listen_commit = service.commit(listen_patch);
    assert(listen_commit.committed && listen_commit.result.ok());
    const uint64_t revision_before_race = service.revision();
    std::size_t ordered_count_before_race = 0;
    {
        std::lock_guard<std::mutex> lock(ordered_observations.mutex);
        ordered_count_before_race = ordered_observations.revisions.size();
    }
    {
        std::lock_guard<std::mutex> lock(g_effect_mutex);
        g_effect_order.clear();
        g_op_enabled = -1;
        g_pause_move_merge = true;
        g_move_merge_entered = false;
        g_release_move_merge = false;
    }

    ModuleConfigPatch older_patch;
    older_patch.move_merge_enabled = true;
    qol::host::CoreSettingsConfigCommit older_commit;
    std::promise<void> older_done_promise;
    auto older_done = older_done_promise.get_future();
    std::thread older_thread([&] {
        older_commit = service.commit(older_patch);
        older_done_promise.set_value();
    });
    {
        std::unique_lock<std::mutex> lock(g_effect_mutex);
        assert(g_effect_changed.wait_for(lock, std::chrono::seconds(2),
                                         [] { return g_move_merge_entered; }));
    }

    ModuleConfigPatch newer_patch;
    newer_patch.op_enabled = false;
    qol::host::CoreSettingsConfigCommit newer_commit;
    std::promise<void> newer_done_promise;
    auto newer_done = newer_done_promise.get_future();
    std::thread newer_thread([&] {
        newer_commit = service.commit(newer_patch);
        newer_done_promise.set_value();
    });
    assert(wait_until([&] { return service.revision() == revision_before_race + 2; }));
    const uint64_t newer_revision = service.revision();

    bool fallback_result = false;
    std::promise<void> fallback_done_promise;
    auto fallback_done = fallback_done_promise.get_future();
    std::thread fallback_thread([&] {
        fallback_result = service.report_bind_failure(newer_revision, 9001);
        fallback_done_promise.set_value();
    });
    assert(wait_until([&] { return service.revision() == newer_revision + 1; }));
    {
        std::lock_guard<std::mutex> lock(g_effect_mutex);
        assert(g_op_enabled == -1);
        assert(g_effect_order.size() == 1 && g_effect_order[0] == "moveMergeEnabled=1");
    }
    {
        std::lock_guard<std::mutex> lock(ordered_observations.mutex);
        assert(ordered_observations.revisions.size() == ordered_count_before_race);
    }
    {
        std::lock_guard<std::mutex> lock(g_effect_mutex);
        g_release_move_merge = true;
        g_pause_move_merge = false;
    }
    g_effect_changed.notify_all();
    if (older_done.wait_for(std::chrono::seconds(3)) != std::future_status::ready ||
        newer_done.wait_for(std::chrono::seconds(3)) != std::future_status::ready ||
        fallback_done.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        std::fprintf(stderr, "ordered runtime commits did not complete\n");
        std::_Exit(3);
    }
    older_thread.join();
    newer_thread.join();
    fallback_thread.join();

    assert(older_commit.committed && older_commit.revision == revision_before_race + 1);
    assert(older_commit.apply_state ==
           qol::contracts::SettingsConfigApplyState::kPendingFrame);
    assert(newer_commit.committed && newer_commit.revision == newer_revision);
    assert(newer_commit.apply_state == qol::contracts::SettingsConfigApplyState::kApplied);
    assert(fallback_result && service.revision() == newer_revision + 1);
    assert(service.snapshot(&config).ok() && config.listen_port == 8088 && !config.op_enabled);
    {
        std::lock_guard<std::mutex> lock(g_effect_mutex);
        assert(g_op_enabled == 0);
        assert(g_effect_order.size() == 2);
        assert(g_effect_order[0] == "moveMergeEnabled=1" && g_effect_order[1] == "op=0");
    }
    {
        std::lock_guard<std::mutex> lock(ordered_observations.mutex);
        assert(ordered_observations.revisions.size() == ordered_count_before_race + 3);
        assert(ordered_observations.revisions[ordered_count_before_race] == older_commit.revision);
        assert(ordered_observations.revisions[ordered_count_before_race + 1] == newer_commit.revision);
        assert(ordered_observations.revisions[ordered_count_before_race + 2] ==
               newer_commit.revision + 1);
    }

    const std::size_t events_before_failure = observations.values.size();
    const uint64_t revision_before_failure = service.revision();
    assert(::unlink((directory + "/config.json").c_str()) == 0);
    assert(::mkdir((directory + "/config.json").c_str(), 0700) == 0);
    qol::features::core_settings::ModuleConfigPatch failed_patch;
    failed_patch.listen_port = 9100;
    const auto failed_commit = service.commit(failed_patch);
    assert(!failed_commit.result.ok() && !failed_commit.committed && !failed_commit.result.persisted);
    assert(failed_commit.revision == revision_before_failure);
    assert(service.revision() == revision_before_failure);
    assert(observations.values.size() == events_before_failure);
    assert(service.snapshot(&config).ok() && config.listen_port == 8088);

    qol::host::owner_identity_reset_for_test();
    qol::infra::config_reset_for_test();
    std::puts("config_platform_service_tests: pass");
}
