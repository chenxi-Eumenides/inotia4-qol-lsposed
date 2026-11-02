#include "host/core_settings_config_service.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {

std::mutex g_runtime_mutex;
std::condition_variable g_runtime_changed;
bool g_runtime_entered = false;
bool g_runtime_release = false;
uint32_t g_runtime_calls = 0;

qol::host::CoreSettingsRuntimeApplyResult throwing_runtime_apply(
    const qol::features::core_settings::ModuleConfig&,
    const qol::features::core_settings::ModuleConfig&) {
    std::unique_lock<std::mutex> lock(g_runtime_mutex);
    const uint32_t call = ++g_runtime_calls;
    if (call == 1) {
        g_runtime_entered = true;
        g_runtime_changed.notify_all();
        if (!g_runtime_changed.wait_for(lock, std::chrono::seconds(3),
                                        [] { return g_runtime_release; }))
            throw std::runtime_error("test runtime wait timed out");
        throw std::runtime_error("injected runtime failure");
    }
    return {qol::contracts::SettingsConfigApplyState::kApplied, 0, "applied"};
}

struct ObserverState {
    std::mutex mutex;
    bool throw_next = false;
    std::vector<uint64_t> surviving_revisions;
};

void throwing_observer(void* context, uint64_t, const qol::features::core_settings::ModuleConfig&) {
    auto& state = *static_cast<ObserverState*>(context);
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.throw_next) {
        state.throw_next = false;
        throw std::runtime_error("injected observer failure");
    }
}

void surviving_observer(void* context, uint64_t revision,
                        const qol::features::core_settings::ModuleConfig&) {
    auto& state = *static_cast<ObserverState*>(context);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.surviving_revisions.push_back(revision);
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string root = std::string(argv[1]) + "/effect-failures-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    const std::string directory = root + "/settings";
    assert(::mkdir(directory.c_str(), 0700) == 0);

    auto& service = qol::host::core_settings_config_service();
    assert(service.initialize(directory).ok());
    const auto seed = service.seed_registered_plugins({"core.settings"});
    assert(seed.result.ok() && seed.durable_commit);
    assert(service.install_runtime_apply(&throwing_runtime_apply));

    ObserverState observers;
    assert(service.subscribe_platform(&throwing_observer, &observers).ok());
    assert(service.subscribe_platform(&surviving_observer, &observers).ok());
    {
        std::lock_guard<std::mutex> lock(observers.mutex);
        observers.throw_next = true;
    }

    qol::features::core_settings::ModuleConfigPatch first_patch;
    first_patch.op_enabled = true;
    std::promise<qol::host::CoreSettingsConfigCommit> first_promise;
    auto first_future = first_promise.get_future();
    std::thread first_thread([&] { first_promise.set_value(service.commit(first_patch)); });
    {
        std::unique_lock<std::mutex> lock(g_runtime_mutex);
        assert(g_runtime_changed.wait_for(lock, std::chrono::seconds(2),
                                          [] { return g_runtime_entered; }));
    }

    qol::features::core_settings::ModuleConfigPatch second_patch;
    second_patch.debug_log_enabled = true;
    std::promise<qol::host::CoreSettingsConfigCommit> second_promise;
    auto second_future = second_promise.get_future();
    std::thread second_thread([&] { second_promise.set_value(service.commit(second_patch)); });
    for (int attempt = 0; attempt < 1000 && service.revision() < 4; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(service.revision() == 4);
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        g_runtime_release = true;
    }
    g_runtime_changed.notify_all();

    const bool first_ready = first_future.wait_for(std::chrono::seconds(3)) ==
                             std::future_status::ready;
    const bool second_ready = second_future.wait_for(std::chrono::seconds(3)) ==
                              std::future_status::ready;
    if (!first_ready || !second_ready) {
        std::fprintf(stderr, "ordered effect dispatcher did not complete both commits\n");
        std::_Exit(2);
    }
    first_thread.join();
    second_thread.join();
    const auto first_commit = first_future.get();
    const auto second_commit = second_future.get();

    assert(first_commit.committed && first_commit.result.ok() && first_commit.result.persisted);
    assert(first_commit.apply_state == qol::contracts::SettingsConfigApplyState::kRejected);
    assert(first_commit.apply_error_code == kQolErrInternal);
    assert(first_commit.apply_message.find("callback threw") != std::string::npos);
    assert(second_commit.committed && second_commit.result.ok() && second_commit.result.persisted);
    assert(second_commit.apply_state == qol::contracts::SettingsConfigApplyState::kApplied);
    assert(second_commit.apply_error_code == 0 && g_runtime_calls == 2);

    qol::features::core_settings::ModuleConfigPatch third_patch;
    third_patch.listen_port = 9000;
    const auto third_commit = service.commit(third_patch);
    assert(third_commit.committed && third_commit.result.persisted &&
           third_commit.apply_state == qol::contracts::SettingsConfigApplyState::kApplied &&
           third_commit.apply_error_code == 0 && g_runtime_calls == 3);

    qol::features::core_settings::ModuleConfig config;
    assert(service.snapshot(&config).ok());
    assert(config.op_enabled && config.debug_log_enabled && config.listen_port == 9000);
    const std::string persisted = read_file(directory + "/config.json");
    assert(persisted.find("\"opEnabled\":true") != std::string::npos);
    assert(persisted.find("\"debugLogEnabled\":true") != std::string::npos);
    assert(persisted.find("\"listenPort\":9000") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(observers.mutex);
        assert(observers.surviving_revisions.size() == 4);
        assert(observers.surviving_revisions[0] == 2);
        assert(observers.surviving_revisions[1] == 3);
        assert(observers.surviving_revisions[2] == 4);
        assert(observers.surviving_revisions[3] == 5);
    }

    std::puts("core_settings_config_service_effect_failures_tests: pass");
    return 0;
}
