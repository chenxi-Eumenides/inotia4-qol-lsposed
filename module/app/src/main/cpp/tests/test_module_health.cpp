#include "contracts/host_sdk.h"
#include "bridge/native/module_health_read.h"
#include "host/module_health.h"
#include "game/state/game_state.h"
#include "plugins/module_health/plugin.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

uint32_t g_tutorial_cancel_calls = 0;

void tutorial_cancel() { ++g_tutorial_cancel_calls; }

namespace {

using qol::contracts::QolModuleHealthBit;
using qol::contracts::QolModuleHealthSnapshot;
uint32_t g_op_query_calls = 0;

QolResult op_disabled(void*, uint8_t* out_enabled) {
    ++g_op_query_calls;
    if (out_enabled == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out_enabled = 0;
    return qol::contracts::ok();
}

QolResult provider_read_failure(void*, qol::contracts::QolModuleHealthSnapshot*) {
    return qol::contracts::error(kQolErrInternal, nullptr, 0);
}

QolModuleHealthSnapshot read_snapshot() {
    QolModuleHealthSnapshot snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const auto& api = qol::host::module_health_facts_api();
    assert(api.read(api.context, &snapshot).ok == 0);
    return snapshot;
}

struct ObserverState {
    std::mutex mutex;
    std::vector<QolModuleHealthSnapshot> snapshots;
};

void record_snapshot(void* context, const QolModuleHealthSnapshot& snapshot) {
    auto* state = static_cast<ObserverState*>(context);
    assert(state != nullptr);
    const QolModuleHealthSnapshot current = read_snapshot();
    assert(current.revision >= snapshot.revision);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->snapshots.push_back(snapshot);
}

struct BlockingObserverState {
    std::mutex mutex;
    std::condition_variable condition;
    bool callback_entered = false;
    bool release_callback = false;
    bool unsubscribe_started = false;
    bool unsubscribe_finished = false;
    uint32_t calls = 0;
};

void block_revision_one(void* context, const QolModuleHealthSnapshot& snapshot) {
    auto* state = static_cast<BlockingObserverState*>(context);
    std::unique_lock<std::mutex> lock(state->mutex);
    ++state->calls;
    if (snapshot.revision != 1) return;
    state->callback_entered = true;
    state->condition.notify_all();
    state->condition.wait(lock, [state] { return state->release_callback; });
}

struct FakeScheduler {
    qol::contracts::QolFrameHandler handler = nullptr;
    void* context = nullptr;
    uint64_t task_id = 0;
    int32_t point = -1;
    int32_t interval = 0;
    int32_t count = -1;
    uint32_t add_calls = 0;
    uint32_t remove_calls = 0;
    bool remove_saw_live_provider = false;
};

FakeScheduler g_scheduler;
uint32_t g_host_fact_reads = 0;

QolResult counted_host_facts_read(void*, QolModuleHealthSnapshot* out) {
    ++g_host_fact_reads;
    const auto& api = qol::host::module_health_facts_api();
    return api.read(api.context, out);
}

QolResult fake_scheduler_add(int32_t point, qol::contracts::QolFrameHandler handler,
                             void* context, int32_t interval, int32_t count,
                             uint64_t* out_id) {
    assert(handler != nullptr && out_id != nullptr);
    ++g_scheduler.add_calls;
    g_scheduler.handler = handler;
    g_scheduler.context = context;
    g_scheduler.point = point;
    g_scheduler.interval = interval;
    g_scheduler.count = count;
    g_scheduler.task_id = 77 + g_scheduler.add_calls;
    *out_id = g_scheduler.task_id;
    return qol::contracts::ok();
}

QolResult fake_scheduler_remove(uint64_t id) {
    assert(id == g_scheduler.task_id);
    ++g_scheduler.remove_calls;
    const auto& descriptor = qol::features::module_health::plugin_descriptor();
    const auto* api = static_cast<const qol::contracts::QolHostModuleHealthFactsApi*>(
        descriptor.provides[0].iface);
    QolModuleHealthSnapshot snapshot{};
    g_scheduler.remove_saw_live_provider = api->read(api->context, &snapshot).ok == 0;
    return qol::contracts::ok();
}

void test_known_failed_recovery_and_sticky_menu() {
    using namespace qol::contracts;
    qol::host::module_health_reset_for_test();

    QolModuleHealthSnapshot snapshot = read_snapshot();
    assert(snapshot.known_mask == 0);
    assert(snapshot.failed_mask == 0);
    assert(snapshot.main_menu_seen == 0);

    qol::host::module_health_set_fact(QolModuleHealthBit::kNative, true, false);
    qol::host::module_health_set_fact(QolModuleHealthBit::kFrameHooks, true, true);
    snapshot = read_snapshot();
    assert(snapshot.known_mask == (static_cast<uint32_t>(QolModuleHealthBit::kNative) |
                                   static_cast<uint32_t>(QolModuleHealthBit::kFrameHooks)));
    assert(snapshot.failed_mask == static_cast<uint32_t>(QolModuleHealthBit::kFrameHooks));

    const uint64_t failed_revision = snapshot.revision;
    qol::host::module_health_set_fact(QolModuleHealthBit::kFrameHooks, true, false);
    snapshot = read_snapshot();
    assert(snapshot.revision > failed_revision);
    assert((snapshot.failed_mask & static_cast<uint32_t>(QolModuleHealthBit::kFrameHooks)) == 0);

    qol::host::module_health_set_fact(QolModuleHealthBit::kNative, false, true);
    snapshot = read_snapshot();
    assert((snapshot.known_mask & static_cast<uint32_t>(QolModuleHealthBit::kNative)) == 0);
    assert((snapshot.failed_mask & static_cast<uint32_t>(QolModuleHealthBit::kNative)) == 0);

    assert(qol::game::is_main_menu_state(4));
    assert(!qol::game::is_main_menu_state(5));
    assert(!qol::game::is_main_menu_state(0xFFFF));
    qol::host::module_health_observe_main_menu(qol::game::is_main_menu_state(5));
    snapshot = read_snapshot();
    assert(snapshot.main_menu_seen == 0);

    qol::host::module_health_observe_main_menu(qol::game::is_main_menu_state(4));
    snapshot = read_snapshot();
    assert(snapshot.main_menu_seen == 1);
    const uint64_t menu_revision = snapshot.revision;
    qol::host::module_health_observe_main_menu(qol::game::is_main_menu_state(5));
    snapshot = read_snapshot();
    assert(snapshot.main_menu_seen == 1);
    assert(snapshot.revision == menu_revision);
    assert(g_tutorial_cancel_calls == 0);

    qol::host::module_health_set_fact(static_cast<QolModuleHealthBit>(1U << 20), true, true);
    snapshot = read_snapshot();
    assert((snapshot.known_mask & ~kQolModuleHealthAllMask) == 0);
    assert((snapshot.failed_mask & ~kQolModuleHealthAllMask) == 0);
}

void test_config_bootstrap_completion_is_atomic() {
    using namespace qol::contracts;
    qol::host::module_health_reset_for_test();
    qol::host::module_health_config_bootstrap_started();
    const uint32_t config_bit = static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage);
    QolModuleHealthSnapshot snapshot = read_snapshot();
    assert((snapshot.known_mask & config_bit) == 0);

    std::atomic<bool> stop{false};
    std::atomic<bool> reader_started{false};
    std::atomic<bool> saw_transient_failure{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const QolModuleHealthSnapshot observed = read_snapshot();
            if ((observed.known_mask & config_bit) != 0 &&
                (observed.failed_mask & config_bit) != 0) {
                saw_transient_failure.store(true, std::memory_order_release);
            }
            reader_started.store(true, std::memory_order_release);
        }
    });
    while (!reader_started.load(std::memory_order_acquire)) std::this_thread::yield();
    qol::host::module_health_config_bootstrap_finished(true);
    for (uint32_t i = 0; i < 100; ++i) (void)read_snapshot();
    stop.store(true, std::memory_order_release);
    reader.join();

    snapshot = read_snapshot();
    assert((snapshot.known_mask & config_bit) != 0);
    assert((snapshot.failed_mask & config_bit) == 0);
    assert(!saw_transient_failure.load(std::memory_order_acquire));

    qol::host::module_health_reset_for_test();
    qol::host::module_health_config_bootstrap_started();
    qol::host::module_health_config_bootstrap_finished(false);
    snapshot = read_snapshot();
    assert((snapshot.known_mask & config_bit) != 0);
    assert((snapshot.failed_mask & config_bit) != 0);
}

void test_host_fact_observers_replay_changes_and_recovery() {
    using namespace qol::contracts;
    qol::host::module_health_reset_for_test();

    const PluginDescriptor& descriptor = qol::features::module_health::plugin_descriptor();
    QolHostSdk unavailable_sdk{};
    PluginContext unavailable_context{sizeof(PluginContext), kHostSdkMajor, kHostSdkMinor,
                                      &unavailable_sdk, nullptr, "module_health", nullptr, 0, 0};
    assert(descriptor.install(unavailable_context) != 0);

    ObserverState observed;
    uint64_t subscription_id = 0;
    assert(qol::host::module_health_subscribe(&record_snapshot, &observed,
                                               &subscription_id).ok == 0);
    assert(subscription_id != 0);
    {
        std::lock_guard<std::mutex> lock(observed.mutex);
        assert(observed.snapshots.size() == 1);
        assert(observed.snapshots.front().revision == 0);
    }

    qol::host::module_health_set_fact(QolModuleHealthBit::kNative, true, true);
    qol::host::module_health_set_fact(QolModuleHealthBit::kNative, true, true);
    qol::host::module_health_config_bootstrap_finished(false);
    qol::host::module_health_config_bootstrap_started();
    qol::host::module_health_config_bootstrap_finished(true);
    qol::host::module_health_observe_main_menu(true);
    qol::host::module_health_observe_main_menu(true);
    qol::host::module_health_set_fact(QolModuleHealthBit::kFrameHooks, true, true);
    {
        std::lock_guard<std::mutex> lock(observed.mutex);
        assert(observed.snapshots.size() == 7);
        for (size_t index = 1; index < observed.snapshots.size(); ++index) {
            assert(observed.snapshots[index].revision > observed.snapshots[index - 1].revision);
        }
        assert((observed.snapshots[1].failed_mask &
                static_cast<uint32_t>(QolModuleHealthBit::kNative)) != 0);
        assert((observed.snapshots[2].failed_mask &
                static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage)) != 0);
        assert((observed.snapshots[3].known_mask &
                static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage)) == 0);
        assert((observed.snapshots[4].known_mask &
                static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage)) != 0);
        assert((observed.snapshots[4].failed_mask &
                static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage)) == 0);
        assert(observed.snapshots[5].main_menu_seen == 1);
        assert((observed.snapshots[6].failed_mask &
                static_cast<uint32_t>(QolModuleHealthBit::kFrameHooks)) != 0);
    }

    assert(qol::host::module_health_unsubscribe(subscription_id).ok == 0);
    const uint32_t config_bit = static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage);
    qol::host::module_health_set_fact(QolModuleHealthBit::kFrameHooks, false, false);
    {
        std::lock_guard<std::mutex> lock(observed.mutex);
        assert(observed.snapshots.size() == 7);
    }
    assert((read_snapshot().failed_mask & config_bit) == 0);
}

void test_unsubscribe_drains_in_flight_callback() {
    using namespace qol::contracts;
    qol::host::module_health_reset_for_test();
    BlockingObserverState observed;
    uint64_t subscription_id = 0;
    assert(qol::host::module_health_subscribe(&block_revision_one, &observed,
                                               &subscription_id).ok == 0);

    std::thread publisher([] {
        qol::host::module_health_set_fact(QolModuleHealthBit::kNative, true, false);
    });
    {
        std::unique_lock<std::mutex> lock(observed.mutex);
        observed.condition.wait(lock, [&observed] { return observed.callback_entered; });
    }

    std::thread unsubscriber([&] {
        {
            std::lock_guard<std::mutex> lock(observed.mutex);
            observed.unsubscribe_started = true;
            observed.condition.notify_all();
        }
        const QolResult result = qol::host::module_health_unsubscribe(subscription_id);
        std::lock_guard<std::mutex> lock(observed.mutex);
        observed.unsubscribe_finished = result.ok == 0;
        observed.condition.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(observed.mutex);
        observed.condition.wait(lock, [&observed] { return observed.unsubscribe_started; });
        assert(!observed.unsubscribe_finished);
        observed.release_callback = true;
        observed.condition.notify_all();
    }
    publisher.join();
    unsubscriber.join();
    {
        std::lock_guard<std::mutex> lock(observed.mutex);
        assert(observed.unsubscribe_finished);
        assert(observed.calls == 2);
    }
    qol::host::module_health_set_fact(QolModuleHealthBit::kFrameHooks, true, true);
    {
        std::lock_guard<std::mutex> lock(observed.mutex);
        assert(observed.calls == 2);
    }
}

void test_resident_host_facts_fallback() {
    using namespace qol::contracts;
    qol::host::module_health_reset_for_test();
    qol::host::module_health_set_fact(QolModuleHealthBit::kNative, true, true);

    QolHostSdk resident_sdk{};
    resident_sdk.abi = {sizeof(resident_sdk), kHostSdkMajor, kHostSdkMinor};
    resident_sdk.module_health = &qol::host::module_health_facts_api();
    QolModuleHealthSnapshot snapshot{};
    assert(qol::bridge::read_module_health_snapshot(nullptr, &resident_sdk, &snapshot).ok == 0);
    const uint32_t native_bit = static_cast<uint32_t>(QolModuleHealthBit::kNative);
    assert((snapshot.known_mask & native_bit) != 0);
    assert((snapshot.failed_mask & native_bit) != 0);

    const QolHostModuleHealthFactsApi failed_provider{
        sizeof(QolHostModuleHealthFactsApi), nullptr, &provider_read_failure};
    snapshot = {};
    assert(qol::bridge::read_module_health_snapshot(&failed_provider, &resident_sdk, &snapshot).ok == 0);
    assert((snapshot.known_mask & native_bit) != 0);
    assert((snapshot.failed_mask & native_bit) != 0);

    QolHostSdk no_resident_sdk{};
    no_resident_sdk.abi = {sizeof(no_resident_sdk), kHostSdkMajor, kHostSdkMinor};
    snapshot = {};
    assert(qol::bridge::read_module_health_snapshot(nullptr, &no_resident_sdk, &snapshot).ok == 0);
    assert(snapshot.known_mask == 0 && snapshot.failed_mask == 0);
}

void test_plugin_is_host_only_and_optional() {
    using namespace qol::contracts;
    const PluginDescriptor& descriptor = qol::features::module_health::plugin_descriptor();
    assert(descriptor.requires_count == 0 && descriptor.requires == nullptr);
    assert(descriptor.optional_count == 0 && descriptor.optional_requires == nullptr);
    assert(descriptor.resources_count == 0 && descriptor.resources == nullptr);
    assert(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    assert(descriptor.http_endpoint_count == 0 && descriptor.http_endpoints == nullptr);
    assert(descriptor.provides_count == 1);
    assert(std::string(descriptor.provides[0].id) == kModuleHealthCapabilityId);
    assert(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    assert(descriptor.feature_count == 1);
    assert(descriptor.features[0].default_enabled == 1);
    assert(descriptor.features[0].category == FeatureCategory::kHidden);

    qol::host::module_health_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_host_fact_reads = 0;
    const QolHostModuleHealthFactsApi counted_api{
        sizeof(QolHostModuleHealthFactsApi), nullptr, &counted_host_facts_read};
    QolHostSdk sdk{};
    sdk.abi = {sizeof(sdk), kHostSdkMajor, kHostSdkMinor};
    sdk.module_health = &counted_api;
    sdk.scheduler = {&fake_scheduler_add, &fake_scheduler_remove};
    assert(sdk.http_dispatch.dispatch == nullptr);
    const QolSecurityApi disabled_security{sizeof(QolSecurityApi), nullptr, &op_disabled};
    sdk.security = &disabled_security;
    PluginContext context{sizeof(PluginContext), kHostSdkMajor, kHostSdkMinor, &sdk, nullptr,
                          "module_health", nullptr, 0, 0};
    assert(descriptor.install(context) == 0);
    const auto* api = static_cast<const QolHostModuleHealthFactsApi*>(descriptor.provides[0].iface);
    QolModuleHealthSnapshot snapshot{};
    assert(api->read(api->context, &snapshot).ok == 0);
    assert(snapshot.struct_size == sizeof(QolModuleHealthSnapshot));
    assert(g_host_fact_reads == 1);
    descriptor.on_enabled(context);
    assert(g_scheduler.add_calls == 1);
    assert(g_scheduler.point == 1);
    assert(g_scheduler.interval == 30);
    assert(g_scheduler.count == 0);
    assert(g_scheduler.task_id != 0);
    qol::host::module_health_set_fact(QolModuleHealthBit::kNative, true, true);
    QolModuleHealthSnapshot cached{};
    assert(api->read(api->context, &cached).ok == 0);
    assert(cached.revision == snapshot.revision);
    assert(g_host_fact_reads == 1);
    assert(g_scheduler.handler(1, g_scheduler.context) == 1);
    assert(g_host_fact_reads == 2);
    assert(api->read(api->context, &cached).ok == 0);
    assert(cached.revision == read_snapshot().revision);
    assert((cached.failed_mask & static_cast<uint32_t>(QolModuleHealthBit::kNative)) != 0);
    assert(g_scheduler.handler(31, g_scheduler.context) == 1);
    assert(g_host_fact_reads == 3);

    descriptor.on_disabled(context);
    assert(g_scheduler.remove_calls == 1);
    assert(g_scheduler.remove_saw_live_provider);
    assert(g_scheduler.handler(61, g_scheduler.context) == 0);
    assert(g_host_fact_reads == 3);
    descriptor.on_enabled(context);
    assert(g_scheduler.add_calls == 2);
    assert(g_op_query_calls == 0);
    assert(api->read(api->context, nullptr).code == kQolErrInvalidArgument);
    descriptor.deactivate(context);
    assert(g_scheduler.remove_calls == 2);
    assert(g_scheduler.remove_saw_live_provider);

    QolHostSdk unavailable_sdk{};
    PluginContext unavailable_context{sizeof(PluginContext), kHostSdkMajor, kHostSdkMinor,
                                      &unavailable_sdk, nullptr, "module_health", nullptr, 0, 0};
    const QolModuleHealthSnapshot before_failed_install = read_snapshot();
    assert(descriptor.install(unavailable_context) != 0);
    const QolModuleHealthSnapshot after_failed_install = read_snapshot();
    assert(after_failed_install.known_mask == before_failed_install.known_mask);
    assert(after_failed_install.failed_mask == before_failed_install.failed_mask);

    assert(module_health_if_available(nullptr) == nullptr);
    sdk.module_health = nullptr;
    assert(module_health_if_available(&sdk) == nullptr);
}

}  // namespace

int main() {
    test_known_failed_recovery_and_sticky_menu();
    test_config_bootstrap_completion_is_atomic();
    test_host_fact_observers_replay_changes_and_recovery();
    test_unsubscribe_drains_in_flight_callback();
    test_resident_host_facts_fallback();
    test_plugin_is_host_only_and_optional();
    std::printf("module_health: all checks passed\n");
    return 0;
}
