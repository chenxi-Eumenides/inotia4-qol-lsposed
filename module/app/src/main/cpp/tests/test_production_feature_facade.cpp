#include "host/features/production.h"
#include "host/features/platform.h"
#include "host/features/http_control.h"
#include "host/owner_identity.h"

#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <mutex>
#include <string>
#include <vector>

#include "infra/status.h"
#include "infra/log/qol_log.h"

using namespace qol::contracts;
using namespace qol::host;
using namespace qol::features::core_settings;

namespace {
int business_calls = 0;
int prepare_calls = 0;
QolResult toggle(PluginContext&, uint8_t) { ++business_calls; return ok(); }
QolResult entry(PluginContext&) { ++business_calls; return ok(); }
constexpr FeatureDefinition definitions[] = {
    {"switch", 0, FeatureCategory::kBasicOptimization, FeatureType::kToggle,
     "switch-name", "switch-description", &toggle, nullptr, nullptr, 6},
    {"open", 1, FeatureCategory::kTools, FeatureType::kEntry,
     "entry-name", "entry-description", nullptr, &entry, "metadata-action", -4},
};
constexpr FeatureDefinition core_custom_ui_features[] = {
    {"core.custom_ui.open_debug_components", 1, FeatureCategory::kHidden,
     FeatureType::kEntry, "打开调试面板", "", nullptr, &entry, "打开"},
    {"core.custom_ui.ui_tools", 1, FeatureCategory::kHidden,
     FeatureType::kToggle, "共享界面工具", "", &toggle, nullptr},
};
constexpr FeatureDefinition core_test_features[] = {
    {"core.test.internal", 1, FeatureCategory::kHidden, FeatureType::kToggle,
     "Core internal feature", "", &toggle, nullptr},
};
constexpr FeatureDefinition settings_features[] = {
    {"page", 1, FeatureCategory::kHidden, FeatureType::kEntry,
     "Settings page", "", nullptr, &entry, "Open"},
};
constexpr FeatureDefinition save_backup_features[] = {
    {"save_backup.open_manager", 1, FeatureCategory::kTools, FeatureType::kEntry,
     "存档备份导出", "打开存档备份管理器。", nullptr, &entry, "打开"},
};
constexpr FeatureDefinition missing_entry_callback[] = {
    {"core.invalid.debug_entry", 1, FeatureCategory::kHidden,
     FeatureType::kEntry, "无回调入口", "", nullptr, nullptr, "打开"},
};
PluginDescriptor descriptor(const char* id) {
    PluginDescriptor out{};
    out.id = id;
    out.features = definitions;
    out.feature_count = 2;
    return out;
}
const PluginDescriptor alpha_descriptor = descriptor("plugin.alpha");
const PluginDescriptor beta_descriptor = descriptor("plugin.beta");
constexpr FeatureDefinition default_off_features[] = {
    {"off_by_default", 0, FeatureCategory::kGameplay, FeatureType::kToggle,
     "Off by default", "", &toggle, nullptr},
};
PluginDescriptor make_default_off_descriptor() {
    auto out = descriptor("plugin.default_off");
    out.features = default_off_features;
    out.feature_count = 1;
    return out;
}
const PluginDescriptor default_off_descriptor = make_default_off_descriptor();
PluginDescriptor make_hidden_core_descriptor() {
    auto out = descriptor("core.test");
    out.features = core_test_features;
    out.feature_count = 1;
    return out;
}
const PluginDescriptor core_descriptor = make_hidden_core_descriptor();
PluginDescriptor make_settings_descriptor() {
    auto out = descriptor("settings");
    out.features = settings_features;
    out.feature_count = 1;
    return out;
}
const PluginDescriptor settings_descriptor = make_settings_descriptor();
constexpr FeatureDefinition core_api_features[] = {
    {"api.enabled", 1, FeatureCategory::kGeneral, FeatureType::kToggle,
     "API service", "", &toggle, nullptr},
};
PluginDescriptor make_visible_core_api_descriptor() {
    auto out = descriptor("core.api");
    out.features = core_api_features;
    out.feature_count = 1;
    return out;
}
const PluginDescriptor core_api_descriptor = make_visible_core_api_descriptor();
PluginDescriptor make_save_backup_descriptor() {
    auto out = descriptor("save_backup");
    out.features = save_backup_features;
    out.feature_count = 1;
    return out;
}
const PluginDescriptor save_backup_descriptor = make_save_backup_descriptor();
PluginDescriptor make_debug_custom_ui_descriptor() {
    auto out = descriptor("core.custom_ui");
    out.features = core_custom_ui_features;
    out.feature_count = sizeof(core_custom_ui_features) / sizeof(core_custom_ui_features[0]);
    return out;
}
const PluginDescriptor core_custom_ui_descriptor = make_debug_custom_ui_descriptor();
PluginDescriptor missing_callback_descriptor() {
    auto out = descriptor("core.invalid_callback");
    out.features = missing_entry_callback;
    out.feature_count = 1;
    return out;
}

QolOwnerToken owner(void* identity, const char* id) {
    assert(owner_register(identity, id));
    QolOwnerToken token = 0;
    uint64_t generation = 0;
    assert(succeeded(owner_identity_api().query(identity, &token, &generation)));
    return token;
}

FeatureReceipt await(const QolFeatureControlApi& api, FeatureReceipt value) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        assert(succeeded(api.receipt(api.context, value.request_id, &value)));
        if (value.persistence != FeaturePersistence::kPending) return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    std::fputs("feature receipt timed out\n", stderr);
    std::abort();
}

struct RuntimeTrace {
    struct Event {
        uint64_t event_revision;
        uint64_t config_revision;
        bool actual_enabled;
        bool desired_enabled;
        std::string plugin_id;
        std::string config_json;
        uint32_t struct_size;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Event> events;
    bool publish_actual_once = false;
    bool block_next = false;
    bool entered = false;
    bool release = false;
};

void runtime_observer(void* raw, const QolPluginRuntimeEffect* effect) {
    assert(raw != nullptr && effect != nullptr);
    auto& trace = *static_cast<RuntimeTrace*>(raw);
    bool publish = false;
    {
        std::unique_lock<std::mutex> lock(trace.mutex);
        trace.events.push_back({effect->event_revision, effect->config_revision,
            effect->actual_enabled != 0, effect->desired_enabled != 0,
            effect->plugin_id ? effect->plugin_id : "",
            std::string(effect->config_json ? effect->config_json : "",
                        effect->config_json_size), effect->struct_size});
        publish = trace.publish_actual_once;
        trace.publish_actual_once = false;
        if (trace.block_next) {
            trace.block_next = false;
            trace.entered = true;
            trace.changed.notify_all();
            assert(trace.changed.wait_for(lock, std::chrono::seconds(4),
                                          [&] { return trace.release; }));
        }
    }
    if (publish) runtime_effects_publish_actual("plugin.alpha", true);
}

void test_abi() {
    static_assert(std::is_standard_layout_v<QolFeatureControlApi> &&
                  std::is_trivial_v<QolFeatureControlApi>);
    static_assert(std::is_standard_layout_v<QolFeatureCatalogItem> &&
                  std::is_trivially_copyable_v<QolFeatureCatalogItem>);
    static_assert(offsetof(QolHostSdk, config_namespace) == 800);
    static_assert(offsetof(QolHostSdk, feature) == 808);
    static_assert(offsetof(QolHostSdk, feature_control) == 816);
    static_assert(static_cast<uint8_t>(FeatureCategory::kGeneral) == 0 &&
                  static_cast<uint8_t>(FeatureCategory::kGameplay) == 1 &&
                  static_cast<uint8_t>(FeatureCategory::kTools) == 2 &&
                  static_cast<uint8_t>(FeatureCategory::kHidden) == 3 &&
                  static_cast<uint8_t>(FeatureCategory::kBasicOptimization) == 4 &&
                  static_cast<uint8_t>(FeatureCategory::kGameInfo) == 5);
    auto feature = production_feature_api();
    auto control = production_feature_control_api();
    uint8_t op_enabled = 1;
    assert(production_security_api().op_enabled(nullptr, &op_enabled).code == kQolErrNotReady);
    assert(op_enabled == 0);
    QolHostSdk sdk{};
    sdk.abi = {sizeof(QolHostSdk), kHostSdkMajor, kHostSdkMinor};
    sdk.feature = &feature;
    assert(feature_if_available(nullptr) == nullptr);
    assert(feature_control_if_available(nullptr) == nullptr);
    assert(feature_if_available(&sdk) == &feature);
    assert(feature_control_if_available(&sdk) == nullptr); // ordinary SDK
    sdk.feature_control = &control;
    assert(feature_control_if_available(&sdk) == &control);
    sdk.abi.minor = 25;
    assert(feature_control_if_available(&sdk) == nullptr);
    assert(!production_feature_bind_management_sdk(sdk));
    sdk.abi.minor = kHostSdkMinor;
    assert(feature_control_if_available(&sdk) == &control);
    sdk.abi.minor = 21;
    assert(feature_if_available(&sdk) == nullptr);
    assert(feature_control_if_available(&sdk) == nullptr);
    assert(!production_feature_bind_management_sdk(sdk));
    assert(!production_feature_bind_runtime_sdk(sdk));
    sdk.abi.minor = 22;
    sdk.abi.major = 2;
    assert(feature_if_available(&sdk) == nullptr);
    assert(feature_control_if_available(&sdk) == nullptr);
    sdk.abi.major = 1;
    sdk.abi.size = offsetof(QolHostSdk, feature);
    assert(feature_if_available(&sdk) == nullptr);
    sdk.abi.size = offsetof(QolHostSdk, feature) + sizeof(sdk.feature) - 1;
    assert(feature_if_available(&sdk) == nullptr);
    sdk.abi.size = offsetof(QolHostSdk, feature_control);
    assert(feature_if_available(&sdk) == &feature);
    assert(feature_control_if_available(&sdk) == nullptr);
    assert(!production_feature_bind_management_sdk(sdk));
    assert(!production_feature_bind_runtime_sdk(sdk));
    sdk.abi.minor = 23;
    sdk.abi.major = 1;
    sdk.abi.size = sizeof(sdk);
    assert(!production_feature_bind_runtime_sdk(sdk));
    sdk.abi.minor = 24;
    assert(production_feature_bind_runtime_sdk(sdk));
    assert(feature_control_if_available(&sdk) == nullptr);
    assert(!production_feature_bind_management_sdk(sdk));
    assert(sdk.runtime_effects == &production_runtime_effects_api());
    assert(sdk.security == &production_security_api());
    sdk.abi.size = offsetof(QolHostSdk, feature_control) + sizeof(sdk.feature_control) - 1;
    assert(feature_control_if_available(&sdk) == nullptr);
    assert(!production_feature_bind_management_sdk(sdk));
    sdk.abi.size = sizeof(sdk);
    sdk.abi.minor = kHostSdkMinor;
    --feature.struct_size;
    assert(feature_if_available(&sdk) == nullptr);
    ++feature.struct_size;
    feature.invoke_entry = nullptr;
    assert(feature_if_available(&sdk) == nullptr);
    feature = production_feature_api();
    control.struct_size = offsetof(QolFeatureControlApi, read_categories) +
                          sizeof(control.read_categories) - 1;
    assert(feature_control_if_available(&sdk) == nullptr);
    control.struct_size = sizeof(control);
    control.read_categories = nullptr;
    assert(feature_control_if_available(&sdk) == nullptr);
    control = production_feature_control_api();
    control.receipt = nullptr;
    assert(feature_control_if_available(&sdk) == nullptr);
    sdk.feature_control = nullptr;
    sdk.abi.minor = kHostSdkMinor;
    assert(production_feature_bind_management_sdk(sdk));
    assert(feature_control_if_available(&sdk) == &production_feature_control_api());
}

void test_runtime_effects(QolOwnerToken alpha) {
    const auto& api = production_runtime_effects_api();
    RuntimeTrace replay;
    replay.publish_actual_once = true;
    uint64_t self_id = 0;
    assert(succeeded(api.subscribe_self(api.context, alpha, &runtime_observer, &replay,
                                        &self_id)));
    assert(self_id != 0);
    {
        std::lock_guard<std::mutex> lock(replay.mutex);
        assert(replay.events.size() == 2);
        assert(replay.events[0].plugin_id == "plugin.alpha" && !replay.events[0].actual_enabled &&
               replay.events[0].desired_enabled);
        assert(replay.events[1].plugin_id == "plugin.alpha" && replay.events[1].actual_enabled &&
               replay.events[1].desired_enabled);
        assert(replay.events[0].event_revision < replay.events[1].event_revision);
        for (const auto& event : replay.events) {
            assert(event.struct_size == sizeof(QolPluginRuntimeEffect));
            assert(event.config_revision != 0 && event.config_json.size() > 1);
        }
    }
    FeatureReceipt config_receipt{};
    const auto& control = production_feature_control_api();
    assert(succeeded(control.request_set_config(control.context, "plugin.alpha", "runtime",
                                                 "\"v\"", 3, &config_receipt)));
    config_receipt = await(control, config_receipt);
    assert(config_receipt.persistence == FeaturePersistence::kPersisted);
    {
        std::lock_guard<std::mutex> lock(replay.mutex);
        assert(replay.events.size() == 3);
        assert(replay.events[2].event_revision > replay.events[1].event_revision);
        assert(replay.events[2].config_revision > replay.events[1].config_revision);
        assert(replay.events[2].desired_enabled);
        assert(replay.events[2].config_json.find("\"runtime\":\"v\"") != std::string::npos);
    }
    FeatureReceipt feature_receipt{};
    assert(succeeded(control.request_set_feature(control.context, "plugin.alpha", "open", 0,
                                                  &feature_receipt)));
    feature_receipt = await(control, feature_receipt);
    assert(feature_receipt.persistence == FeaturePersistence::kPersisted);
    {
        std::lock_guard<std::mutex> lock(replay.mutex);
        assert(replay.events.size() == 4);
        assert(!replay.events[3].desired_enabled && replay.events[3].actual_enabled);
    }
    runtime_effects_publish_actual("plugin.alpha", false);
    {
        std::lock_guard<std::mutex> lock(replay.mutex);
        assert(replay.events.size() == 5);
        assert(!replay.events[4].desired_enabled && !replay.events[4].actual_enabled);
    }
    assert(succeeded(control.request_set_feature(control.context, "plugin.alpha", "open", 1,
                                                  &feature_receipt)));
    feature_receipt = await(control, feature_receipt);
    assert(feature_receipt.persistence == FeaturePersistence::kPersisted);
    {
        std::lock_guard<std::mutex> lock(replay.mutex);
        assert(replay.events.size() == 6);
        assert(replay.events[5].desired_enabled && !replay.events[5].actual_enabled);
    }
    assert(succeeded(api.unsubscribe(api.context, alpha, self_id)));
    runtime_effects_publish_actual("plugin.alpha", false);
    {
        std::lock_guard<std::mutex> lock(replay.mutex);
        assert(replay.events.size() == 6);
    }

    RuntimeTrace parallel;
    uint64_t parallel_id = 0;
    assert(succeeded(runtime_effects_subscribe_trusted("plugin.alpha", &runtime_observer,
                                                       &parallel, &parallel_id)));
    std::vector<std::thread> publishers;
    for (uint32_t index = 0; index < 8; ++index) {
        publishers.emplace_back([index] {
            runtime_effects_publish_actual("plugin.alpha", (index % 2) == 0);
        });
    }
    for (auto& publisher : publishers) publisher.join();
    assert(succeeded(runtime_effects_unsubscribe_trusted(parallel_id)));
    {
        std::lock_guard<std::mutex> lock(parallel.mutex);
        assert(!parallel.events.empty());
        for (std::size_t index = 1; index < parallel.events.size(); ++index)
            assert(parallel.events[index - 1].event_revision < parallel.events[index].event_revision);
    }

}

void test_trusted_runtime_effects_survive_topic_owner_stop(QolOwnerToken core_api) {
    const auto& api = production_runtime_effects_api();
    const auto& control = production_feature_control_api();
    RuntimeTrace host_trace;
    auto plugin_trace = std::make_unique<RuntimeTrace>();
    uint64_t host_id = 0;
    uint64_t plugin_id = 0;
    assert(succeeded(runtime_effects_subscribe_trusted(
        "core.api", &runtime_observer, &host_trace, &host_id)));
    assert(succeeded(api.subscribe_self(api.context, core_api, &runtime_observer,
                                        plugin_trace.get(), &plugin_id)));
    assert(host_id != 0 && plugin_id != 0);
    {
        std::lock_guard<std::mutex> lock(host_trace.mutex);
        assert(host_trace.events.size() == 1 && host_trace.events[0].plugin_id == "core.api");
    }
    {
        std::lock_guard<std::mutex> lock(plugin_trace->mutex);
        assert(plugin_trace->events.size() == 1 &&
               plugin_trace->events[0].plugin_id == "core.api");
    }
    // A plugin token cannot unsubscribe the Host-lifetime observer sharing its topic.
    assert(api.unsubscribe(api.context, core_api, host_id).code == kQolErrBlocked);

    bool current_actual = false;
    {
        std::lock_guard<std::mutex> lock(host_trace.mutex);
        current_actual = host_trace.events.back().actual_enabled;
    }
    const auto current_plugin_event_count = [&] {
        std::lock_guard<std::mutex> lock(plugin_trace->mutex);
        return plugin_trace->events.size();
    };
    const auto current_host_event_count = [&] {
        std::lock_guard<std::mutex> lock(host_trace.mutex);
        return host_trace.events.size();
    };
    const auto assert_monotonic_revisions = [](RuntimeTrace& trace) {
        std::lock_guard<std::mutex> lock(trace.mutex);
        for (std::size_t index = 1; index < trace.events.size(); ++index)
            assert(trace.events[index - 1].event_revision < trace.events[index].event_revision);
    };
    runtime_effects_publish_actual("core.api", !current_actual);
    current_actual = !current_actual;
    assert(current_host_event_count() == 2 && current_plugin_event_count() == 2);
    runtime_effects_publish_actual("core.api", !current_actual);
    current_actual = !current_actual;
    assert(current_host_event_count() == 3 && current_plugin_event_count() == 3);

    const auto publish_config = [&](const char* value) {
        FeatureReceipt receipt{};
        assert(succeeded(control.request_set_config(control.context, "core.api", "runtime_probe",
                                                     value, static_cast<uint32_t>(std::strlen(value)),
                                                     &receipt)));
        receipt = await(control, receipt);
        assert(receipt.persistence == FeaturePersistence::kPersisted);
    };
    publish_config("\"host-lifetime\"");
    assert(current_host_event_count() == 4 && current_plugin_event_count() == 4);

    // Core API owner stop drains its own callback before its context is destroyed, but the
    // Host-lifetime observer remains subscribed to the same plugin-id topic.
    {
        std::lock_guard<std::mutex> lock(plugin_trace->mutex);
        plugin_trace->block_next = true;
    }
    const bool blocked_actual = !current_actual;
    std::thread publisher([blocked_actual] {
        runtime_effects_publish_actual("core.api", blocked_actual);
    });
    {
        std::unique_lock<std::mutex> lock(plugin_trace->mutex);
        assert(plugin_trace->changed.wait_for(lock, std::chrono::seconds(3),
                                              [&] { return plugin_trace->entered; }));
    }
    std::atomic<bool> owner_stop_done{false};
    std::thread owner_stopper([&] {
        runtime_effects_unsubscribe_plugin("core.api");
        owner_stop_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!owner_stop_done.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(plugin_trace->mutex);
        plugin_trace->release = true;
    }
    plugin_trace->changed.notify_all();
    publisher.join();
    owner_stopper.join();
    assert(owner_stop_done.load(std::memory_order_acquire));
    current_actual = blocked_actual;
    assert(current_plugin_event_count() == 5);
    assert_monotonic_revisions(*plugin_trace);
    plugin_trace.reset();

    runtime_effects_publish_actual("core.api", !current_actual);
    current_actual = !current_actual;
    assert(current_host_event_count() == 6);
    publish_config("\"host-still-subscribed\"");
    assert(current_host_event_count() == 7);

    // Rebind after owner activation must produce one callback per event, not duplicate rows.
    RuntimeTrace rebound_plugin_trace;
    uint64_t rebound_plugin_id = 0;
    assert(succeeded(api.subscribe_self(api.context, core_api, &runtime_observer,
                                        &rebound_plugin_trace, &rebound_plugin_id)));
    assert(rebound_plugin_id != 0);
    {
        std::lock_guard<std::mutex> lock(rebound_plugin_trace.mutex);
        assert(rebound_plugin_trace.events.size() == 1);
    }
    runtime_effects_publish_actual("core.api", !current_actual);
    current_actual = !current_actual;
    assert(current_host_event_count() == 8);
    {
        std::lock_guard<std::mutex> lock(rebound_plugin_trace.mutex);
        assert(rebound_plugin_trace.events.size() == 2);
    }
    runtime_effects_unsubscribe_plugin("core.api");
    const std::size_t host_events_before_unsubscribe = current_host_event_count();
    assert(succeeded(runtime_effects_unsubscribe_trusted(host_id)));
    runtime_effects_publish_actual("core.api", !current_actual);
    publish_config("\"host-unsubscribed\"");
    assert(current_host_event_count() == host_events_before_unsubscribe);
    assert_monotonic_revisions(host_trace);
    {
        std::lock_guard<std::mutex> lock(rebound_plugin_trace.mutex);
        assert(rebound_plugin_trace.events.size() == 2);
        for (std::size_t index = 1; index < rebound_plugin_trace.events.size(); ++index)
            assert(rebound_plugin_trace.events[index - 1].event_revision <
                   rebound_plugin_trace.events[index].event_revision);
    }

    auto draining_host_trace = std::make_unique<RuntimeTrace>();
    uint64_t draining_host_id = 0;
    assert(succeeded(runtime_effects_subscribe_trusted(
        "core.api", &runtime_observer, draining_host_trace.get(), &draining_host_id)));
    bool draining_actual = false;
    {
        std::lock_guard<std::mutex> lock(draining_host_trace->mutex);
        assert(draining_host_trace->events.size() == 1);
        draining_actual = draining_host_trace->events.back().actual_enabled;
        draining_host_trace->block_next = true;
    }
    std::thread trusted_publisher([draining_actual] {
        runtime_effects_publish_actual("core.api", !draining_actual);
    });
    {
        std::unique_lock<std::mutex> lock(draining_host_trace->mutex);
        assert(draining_host_trace->changed.wait_for(lock, std::chrono::seconds(3),
                                                     [&] { return draining_host_trace->entered; }));
    }
    std::atomic<bool> trusted_unsubscribe_done{false};
    std::thread trusted_unsubscriber([&] {
        assert(succeeded(runtime_effects_unsubscribe_trusted(draining_host_id)));
        trusted_unsubscribe_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!trusted_unsubscribe_done.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(draining_host_trace->mutex);
        draining_host_trace->release = true;
    }
    draining_host_trace->changed.notify_all();
    trusted_publisher.join();
    trusted_unsubscriber.join();
    assert(trusted_unsubscribe_done.load(std::memory_order_acquire));
    std::size_t trusted_events_after_drain = 0;
    {
        std::lock_guard<std::mutex> lock(draining_host_trace->mutex);
        trusted_events_after_drain = draining_host_trace->events.size();
        assert(trusted_events_after_drain == 2);
        assert(draining_host_trace->events[0].event_revision <
               draining_host_trace->events[1].event_revision);
    }
    draining_host_trace.reset();
    runtime_effects_publish_actual("core.api", draining_actual);
    assert(trusted_events_after_drain == 2);
}

void test_seeded_runtime_effects() {
    const auto& control = production_feature_control_api();
    uint8_t desired_enable = 0;
    uint8_t any_feature = 1;
    assert(succeeded(control.read_desired(control.context, "plugin.default_off",
                                          &desired_enable, &any_feature)));
    assert(desired_enable == 1 && any_feature == 0);

    RuntimeTrace trace;
    uint64_t subscription_id = 0;
    assert(succeeded(runtime_effects_subscribe_trusted("plugin.default_off", &runtime_observer,
                                                       &trace, &subscription_id)));
    assert(subscription_id != 0);
    {
        std::lock_guard<std::mutex> lock(trace.mutex);
        assert(trace.events.size() == 1);
        assert(!trace.events[0].actual_enabled && !trace.events[0].desired_enabled);
    }

    FeatureReceipt receipt{};
    assert(succeeded(control.request_set_feature(control.context, "plugin.default_off",
                                                  "off_by_default", 1, &receipt)));
    receipt = await(control, receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted);
    {
        std::lock_guard<std::mutex> lock(trace.mutex);
        assert(trace.events.size() == 2);
        assert(!trace.events[1].actual_enabled && trace.events[1].desired_enabled);
    }

    assert(succeeded(control.request_set_feature(control.context, "plugin.default_off",
                                                  "off_by_default", 0, &receipt)));
    receipt = await(control, receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted);
    {
        std::lock_guard<std::mutex> lock(trace.mutex);
        assert(trace.events.size() == 3);
        assert(!trace.events[2].actual_enabled && !trace.events[2].desired_enabled);
    }
    assert(succeeded(runtime_effects_unsubscribe_trusted(subscription_id)));
}
}  // namespace

// Focused registry seam: no resource engine or product callbacks are installed.
// Inactive contexts never become business-ready; only admission/queue behavior is tested.
namespace qol::host {
uint32_t plugin_count() { return 8; }
const contracts::PluginDescriptor* find_plugin(const char* id) {
    if (id && std::strcmp(id, "plugin.alpha") == 0) return &alpha_descriptor;
    if (id && std::strcmp(id, "plugin.beta") == 0) return &beta_descriptor;
    if (id && std::strcmp(id, "plugin.default_off") == 0) return &default_off_descriptor;
    if (id && std::strcmp(id, "core.test") == 0) return &core_descriptor;
    if (id && std::strcmp(id, "core.api") == 0) return &core_api_descriptor;
    if (id && std::strcmp(id, "settings") == 0) return &settings_descriptor;
    if (id && std::strcmp(id, "core.custom_ui") == 0) return &core_custom_ui_descriptor;
    if (id && std::strcmp(id, "save_backup") == 0) return &save_backup_descriptor;
    return nullptr;
}
bool feature_registry_prepare(const HostDeps&) { ++prepare_calls; return true; }
void feature_registry_bind_desired_request(FeatureDesiredRequest, void*) {}
uint32_t feature_registry_order(const char**, uint32_t) { return 0; }
bool feature_registry_dependencies_wanted(const char*, FeatureWants, void*) { return true; }
bool feature_registry_dependencies_ready(const char*) { return true; }
bool feature_registry_busy(const char*) { return false; }
bool feature_registry_active(const char*) { return false; }
bool feature_registry_transition(const char*, bool) { return false; }
bool feature_registry_context(const char*, contracts::PluginContext*) { return false; }
void feature_registry_set_wanted(const char*, bool) {}
uint32_t feature_registry_step() { return 0; }
bool feature_registry_owner_ready(const char*) { return true; }
}  // namespace qol::host

void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {}
namespace {
bool g_debug_logging_enabled = false;
}
void qol_log_set_debug_enabled(bool enabled) { g_debug_logging_enabled = enabled; }

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string path = std::string(argv[1]) + "/production-feature-XXXXXX";
    assert(::mkdtemp(path.data()));
    const auto file_path = path + "/config.json";
    struct stat stat_buffer{};
    assert(::stat(file_path.c_str(), &stat_buffer) != 0);
    assert(core_settings_config_service().state() == CoreSettingsConfigServiceState::kUninitialized);
    test_abi();
    assert(&production_feature_controller().api() == &production_feature_api());
    assert(&production_feature_reconciler() == &production_feature_reconciler());
    assert(production_feature_directory().definitions().empty());
    assert(core_settings_config_service().revision() == 0);
    assert(core_settings_config_service().state() == CoreSettingsConfigServiceState::kUninitialized);
    assert(::stat(file_path.c_str(), &stat_buffer) != 0 && prepare_calls == 0);
    const auto& control = production_feature_control_api();
    const FeatureCategoryDefinition* categories = nullptr;
    uint32_t category_count = 0;
    assert(succeeded(control.read_categories(control.context, &categories, &category_count)));
    assert(category_count == 6 && categories != nullptr);
    const FeatureCategoryDefinition expected_categories[] = {
        {FeatureCategory::kGeneral, "基础设置", -8},
        {FeatureCategory::kBasicOptimization, "基础优化设置", -7},
        {FeatureCategory::kGameplay, "更多优化功能", -5},
        {FeatureCategory::kTools, "更多工具", -3},
        {FeatureCategory::kHidden, "调试开关", -10},
        {FeatureCategory::kGameInfo, "游戏信息", -9},
    };
    for (uint32_t index = 0; index < category_count; ++index) {
        assert(categories[index].category == expected_categories[index].category);
        assert(std::strcmp(categories[index].name, expected_categories[index].name) == 0);
        assert(categories[index].display_priority == expected_categories[index].display_priority);
    }
    assert(control.read_categories(control.context, nullptr, &category_count).code ==
           kQolErrInvalidArgument);
    assert(control.read_categories(control.context, &categories, nullptr).code ==
           kQolErrInvalidArgument);
    const QolFeatureCatalogPlugin* catalog = nullptr;
    uint32_t count = 0;
    assert(control.read_catalog(control.context, &catalog, &count).code == kQolErrNotReady);
    assert(!production_feature_bind_frame({}));
    auto missing = alpha_descriptor;
    missing.feature_count = 0;
    assert(!production_feature_register(missing).ok());
    assert(production_feature_directory().definitions().empty());
    assert(production_feature_register(alpha_descriptor).ok());
    assert(production_feature_register(beta_descriptor).ok());
    assert(!production_feature_register(alpha_descriptor).ok());
    assert(production_feature_register_host_owner(platform_config_descriptor()).ok());
    assert(production_feature_register_host_owner(security_config_descriptor()).ok());
    assert(production_feature_register(core_descriptor).ok());
    assert(production_feature_register(settings_descriptor).ok());
    assert(production_feature_register(core_api_descriptor).ok());
    const auto missing_callback = missing_callback_descriptor();
    const size_t definitions_before_invalid = production_feature_directory().definitions().size();
    assert(!production_feature_register(missing_callback).ok());
    assert(production_feature_directory().definitions().size() == definitions_before_invalid);
    assert(production_feature_register(core_custom_ui_descriptor).ok());
    assert(production_feature_register(default_off_descriptor).ok());
    assert(production_feature_register(save_backup_descriptor).ok());
    {
        std::ofstream file(file_path);
        file << R"({"module.platform":{"enable":true,"features":{"op.enabled":false,"debug.logging":false},"config":{}}})";
    }
    assert(production_feature_bootstrap(path).ok());
    assert(!production_feature_register(descriptor("late")).ok());
    assert(!production_feature_register_host_owner(platform_config_descriptor()).ok());
    assert(!production_feature_bootstrap(path).ok());
    assert(succeeded(control.read_catalog(control.context, &catalog, &count)) && count == 9);
    assert(std::strcmp(catalog[2].plugin_id, kPlatformConfigOwner) == 0 && catalog[2].host_owned == 1);
    assert(catalog[2].feature_count == 1);
    assert(std::strcmp(catalog[2].features[0].id, "debug.logging") == 0);
    uint8_t feature_value = 1;
    assert(succeeded(control.read_feature(control.context, kPlatformConfigOwner,
                                          "debug.logging", &feature_value)) && !feature_value);
    assert(succeeded(control.read_feature(control.context, kSecurityConfigOwner,
                                          "op.enabled", &feature_value)) && !feature_value);
    uint8_t op_enabled = 1;
    assert(succeeded(production_security_api().op_enabled(nullptr, &op_enabled)));
    assert(op_enabled == 0);
    assert(catalog[0].host_owned == 0);
    assert(std::strcmp(catalog[0].plugin_id, "plugin.alpha") == 0);
    assert(catalog[0].feature_count == 2);
    assert(std::strcmp(catalog[0].features[0].id, "switch") == 0);
    assert(catalog[0].features[0].display_priority == 6);
    assert(production_feature_directory().definitions()[0].features[0].on_toggle == &toggle);
    assert(catalog[0].features[1].type == FeatureType::kEntry);
    assert(catalog[0].features[1].display_priority == -4);
    assert(catalog[0].features[1].default_enabled == 1);
    assert(production_feature_directory().definitions()[0].features[1].on_entry == &entry);
    assert(std::strcmp(catalog[0].features[1].description, "entry-description") == 0);
    assert(std::strcmp(production_feature_directory().definitions()[0].features[1].action_label,
                       "metadata-action") == 0);
    assert(std::strcmp(production_feature_controller().catalog()[0].features[1].action_label,
                       "metadata-action") == 0);
    assert(std::strcmp(catalog[0].features[1].action_label, "metadata-action") == 0);
    assert(production_feature_directory().definitions()[0].features[0].action_label == nullptr);
    assert(catalog[0].features[0].action_label == nullptr);
    // Every registered descriptor contributes to the player catalog except the settings page;
    // category filtering belongs to the settings model, not plugin identity or metadata opt-in.
    assert(production_feature_directory().definitions().size() == 10);
    uint8_t core_enabled = 0;
    assert(succeeded(control.read_feature(control.context, "core.test", "core.test.internal", &core_enabled)) && core_enabled == 1);
    assert(succeeded(control.read_feature(control.context, "settings", "page", &core_enabled)) && core_enabled == 1);
    assert(count == 9);
    const auto catalog_for = [catalog, count](const char* id) -> const QolFeatureCatalogPlugin* {
        for (uint32_t index = 0; index < count; ++index) {
            if (std::strcmp(catalog[index].plugin_id, id) == 0) return &catalog[index];
        }
        return nullptr;
    };
    const auto* security_catalog = catalog_for("module.security");
    assert(security_catalog != nullptr && security_catalog->host_owned == 1 &&
           security_catalog->feature_count == 1);
    assert(std::strcmp(security_catalog->features[0].id, "op.enabled") == 0 &&
           security_catalog->features[0].category == FeatureCategory::kGeneral &&
           security_catalog->features[0].default_enabled == 0);

    const auto* core_test_catalog = catalog_for("core.test");
    assert(core_test_catalog != nullptr && core_test_catalog->feature_count == 1);
    assert(core_test_catalog->features[0].category == FeatureCategory::kHidden &&
           std::strcmp(core_test_catalog->features[0].description, "") == 0);

    const auto* api_catalog = catalog_for("core.api");
    assert(api_catalog != nullptr && api_catalog->host_owned == 0 &&
           api_catalog->feature_count == 1);
    assert(std::strcmp(api_catalog->features[0].id, "api.enabled") == 0 &&
           api_catalog->features[0].category == FeatureCategory::kGeneral &&
           api_catalog->features[0].default_enabled == 1 &&
           std::strcmp(api_catalog->features[0].description, "") == 0);

    const auto* custom_ui_catalog = catalog_for("core.custom_ui");
    assert(custom_ui_catalog != nullptr && custom_ui_catalog->host_owned == 0 &&
           custom_ui_catalog->feature_count == 2);
    const auto& debug_entry = custom_ui_catalog->features[0];
    assert(std::strcmp(debug_entry.id, "core.custom_ui.open_debug_components") == 0);
    assert(debug_entry.type == FeatureType::kEntry &&
           debug_entry.category == FeatureCategory::kHidden && debug_entry.default_enabled == 1);
    assert(std::strcmp(debug_entry.action_label, "打开") == 0 &&
           std::strcmp(debug_entry.description, "") == 0);
    assert(std::strcmp(custom_ui_catalog->features[1].id, "core.custom_ui.ui_tools") == 0 &&
           custom_ui_catalog->features[1].category == FeatureCategory::kHidden);
    for (uint32_t index = 0; index < count; ++index) {
        if (std::strncmp(catalog[index].plugin_id, "core.", 5) != 0) continue;
        for (uint32_t feature = 0; feature < catalog[index].feature_count; ++feature) {
            const bool api_service = std::strcmp(catalog[index].plugin_id, "core.api") == 0 &&
                std::strcmp(catalog[index].features[feature].id, "api.enabled") == 0;
            assert(catalog[index].features[feature].category ==
                   (api_service ? FeatureCategory::kGeneral : FeatureCategory::kHidden));
            assert(catalog[index].features[feature].description != nullptr &&
                   catalog[index].features[feature].description[0] == '\0');
        }
    }
    const auto* settings_catalog = catalog_for("settings");
    assert(settings_catalog == nullptr);
    const auto* backup_catalog = catalog_for("save_backup");
    assert(backup_catalog != nullptr && backup_catalog->feature_count == 1);
    assert(std::strcmp(backup_catalog->features[0].id, "save_backup.open_manager") == 0 &&
           backup_catalog->features[0].type == FeatureType::kEntry &&
           backup_catalog->features[0].category == FeatureCategory::kTools &&
           backup_catalog->features[0].default_enabled == 1 &&
           std::strcmp(backup_catalog->features[0].action_label, "打开") == 0);
    for (uint32_t i = 0; i < count; ++i) {
        assert(std::strcmp(catalog[i].plugin_id, "settings") != 0);
    }

    int alpha_identity = 0, manager_identity = 0, core_api_identity = 0;
    const auto alpha = owner(&alpha_identity, "plugin.alpha");
    const auto manager = owner(&manager_identity, "plugin.manager");
    const auto core_api = owner(&core_api_identity, "core.api");
    test_seeded_runtime_effects();
    test_runtime_effects(alpha);
    test_trusted_runtime_effects_survive_topic_owner_stop(core_api);
    const auto& api = production_feature_api();
    char buffer[8192]{};
    uint32_t size = 0;
    uint8_t enabled = 9, any = 9;
    FeatureReceipt receipt{};
    assert(succeeded(api.read_namespace(api.context, alpha, "plugin.alpha", buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size).find("\"enable\"") == std::string::npos);
    assert(api.read_feature(api.context, alpha, "plugin.beta", "open", &enabled).code == kQolErrBlocked);
    assert(api.request_set_feature(api.context, alpha, "plugin.beta", "open", 0, &receipt).code == kQolErrBlocked);
    assert(api.request_desired(api.context, alpha, "plugin.alpha", 0, &receipt).code == kQolErrBlocked);
    assert(api.read_desired(api.context, alpha, "plugin.alpha", &enabled, &any).code == kQolErrBlocked);
    assert(api.read_catalog(api.context, manager, buffer, sizeof(buffer), &size).code == kQolErrBlocked);
    assert(succeeded(production_feature_controller().grant_admin(manager)));
    assert(succeeded(api.read_catalog(api.context, manager, buffer, sizeof(buffer), &size)));
    const std::string expected_plugin = R"([{"id":"switch","name":"switch-name","description":"switch-description","type":"toggle","category":4,"default":false},{"id":"open","name":"entry-name","description":"entry-description","type":"entry","category":2,"default":true}])";
    const std::string catalog_json(buffer, size);
    assert(catalog_json.rfind("{\"plugin.alpha\":" + expected_plugin + ",\"plugin.beta\":" + expected_plugin + ",\"module.platform\":[", 0) == 0);
    assert(catalog_json.find("\"core.api\":[{\"id\":\"api.enabled\"") != std::string::npos &&
           catalog_json.find("\"module.security\":[{\"id\":\"op.enabled\"") != std::string::npos &&
           catalog_json.find("\"debug.logging\"") != std::string::npos);

    assert(api.invoke_entry(api.context, alpha, "plugin.alpha", "open").code == kQolErrNotReady);
    assert(production_feature_bind_frame({}) && prepare_calls == 1);
    assert(succeeded(control.invoke_entry(control.context, "plugin.alpha", "open")));
    assert(business_calls == 0); // admission never executes on this caller
    production_feature_frame_step();
    assert(business_calls == 0); // inactive registry context guard
    owner_suspend(&alpha_identity);
    owner_suspend(&manager_identity);
    assert(api.read_feature(api.context, alpha, "plugin.alpha", "open", &enabled).code != kQolOk);
    assert(api.read_catalog(api.context, manager, buffer, sizeof(buffer), &size).code != kQolOk);
     assert(succeeded(control.read_catalog(control.context, &catalog, &count)) && count == 9);
    assert(succeeded(control.read_desired(control.context, "plugin.alpha", &enabled, &any)) && enabled && any);
    assert(succeeded(control.request_set_feature(control.context, "plugin.alpha", "open", 0, &receipt)));
    const auto host_receipt = receipt.request_id;
    receipt = await(control, receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.read_desired(control.context, "plugin.alpha", &enabled, &any)) && !enabled && !any);
    assert(control.invoke_entry(control.context, "plugin.alpha", "open").code == kQolErrBlocked);
    assert(succeeded(control.request_desired(control.context, "plugin.alpha", 0, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.request_set_feature(control.context, "plugin.alpha", "open", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.read_desired(control.context, "plugin.alpha", &enabled, &any)) && enabled && any);
    assert(succeeded(control.request_desired(control.context, "plugin.alpha", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.receipt(control.context, host_receipt, &receipt)));
    assert(owner_resume(&alpha_identity));
    QolOwnerToken refreshed = 0;
    uint64_t generation = 0;
    assert(succeeded(owner_identity_api().query(&alpha_identity, &refreshed, &generation)));
    assert(refreshed != alpha);
    assert(api.receipt(api.context, refreshed, host_receipt, &receipt).code == kQolErrBlocked);
    assert(succeeded(api.request_set_config(api.context, refreshed, "plugin.alpha", "parameter", "3", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(api.read_config(api.context, refreshed, "plugin.alpha", "parameter", buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size) == "3");
    assert(business_calls == 0);

    // Host-owned platform owner: resident, actual always enabled, desired toggles rejected.
    int32_t actual_state = -1;
    uint8_t actual_enabled = 9;
    assert(succeeded(control.read_actual(control.context, kPlatformConfigOwner, &actual_state, &actual_enabled)));
    assert(actual_state == static_cast<int32_t>(qol::infra::FeatureState::kEnabled) && actual_enabled == 1);
    assert(control.read_actual(control.context, kPlatformConfigOwner, &actual_state, &actual_enabled).code == kQolOk);
    assert(control.request_desired(control.context, kPlatformConfigOwner, 0, &receipt).code == kQolErrUnsupported);

    // OP permission is a separate Host owner and never projects into the platform logging feature.
    assert(succeeded(control.read_desired(control.context, kPlatformConfigOwner, &enabled, &any)) && enabled && !any);
    assert(succeeded(control.request_set_feature(control.context, kSecurityConfigOwner,
                                                  "op.enabled", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(production_security_api().op_enabled(nullptr, &op_enabled)) && op_enabled);
    assert(succeeded(control.read_feature(control.context, kPlatformConfigOwner,
                                          "debug.logging", &feature_value)) && !feature_value);
    assert(succeeded(control.read_actual(control.context, kSecurityConfigOwner, &actual_state, &actual_enabled)) &&
           actual_state == static_cast<int32_t>(qol::infra::FeatureState::kEnabled) && actual_enabled == 1);
    assert(control.request_set_feature(control.context, kPlatformConfigOwner,
                                       "api.enabled", 1, &receipt).code == kQolErrNotFound);
    assert(succeeded(control.request_set_feature(control.context, kPlatformConfigOwner,
                                                  "debug.logging", 1, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(control.read_feature(control.context, kPlatformConfigOwner,
                                          "debug.logging", &feature_value)) && feature_value);
    assert(succeeded(production_security_api().op_enabled(nullptr, &op_enabled)) && op_enabled);
    assert(feature_platform_state().debug_logging && g_debug_logging_enabled);
    assert(succeeded(control.request_set_feature(control.context, kSecurityConfigOwner,
                                                  "op.enabled", 0, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(production_security_api().op_enabled(nullptr, &op_enabled)) && !op_enabled);
    assert(feature_platform_state().debug_logging && g_debug_logging_enabled);
    assert(succeeded(control.request_set_feature(control.context, kPlatformConfigOwner,
                                                  "debug.logging", 0, &receipt)));
    assert(await(control, receipt).persistence == FeaturePersistence::kPersisted);
    assert(!feature_platform_state().debug_logging && !g_debug_logging_enabled);

    // Host-owned HTTP control routes are resident and settings-independent.
    auto invoke = [](HttpMethod method, const std::string& path, const std::string& body_text) {
        thread_local std::vector<uint8_t> response(65536, 0);
        std::fill(response.begin(), response.end(), 0);
        const auto* body = body_text.empty() ? nullptr : reinterpret_cast<const uint8_t*>(body_text.data());
        HttpReq request{method, path.c_str(), static_cast<uint32_t>(path.size()), body,
                                   static_cast<uint32_t>(body_text.size()), nullptr, 0};
        HttpRes out{response.data(), static_cast<uint32_t>(response.size()), 0, 0};
        assert(feature_control_http_route(request));
        (void)feature_control_http_dispatch(request, out);
        return std::make_pair(out.status, std::string(reinterpret_cast<const char*>(response.data()), out.body_size));
    };
    {
        const auto list = invoke(HttpMethod::kGet, "/api/config/list", "");
        assert(list.first == 200 && list.second.find("\"enable\"") != std::string::npos && list.second.find("plugin.alpha") != std::string::npos);
        assert(list.second.find("\"op.enabled\":false") != std::string::npos);
        // Projection object key order matches the on-disk schema: enable, features, config.
        const auto alpha_object = list.second.find("\"plugin.alpha\":{");
        assert(alpha_object != std::string::npos);
        assert(list.second.compare(alpha_object + std::strlen("\"plugin.alpha\":{"),
                                   std::strlen("\"enable\":"), "\"enable\":") == 0);
        const auto set_feature = invoke(HttpMethod::kPost, "/api/config/set",
            "{\"plugin_id\":\"plugin.alpha\",\"field\":\"feature\",\"key\":\"switch\",\"value\":false}");
        assert(set_feature.first == 200 && set_feature.second.find("\"persistence\":\"pending\"") != std::string::npos);
        const auto lifecycle = invoke(HttpMethod::kPost, "/api/op/features/lifecycle",
            "{\"id\":\"plugin.alpha\",\"action\":\"activate\"}");
        assert(lifecycle.first == 200 && lifecycle.second.find("\"admission\":\"admitted\"") != std::string::npos);
        const auto unknown_field = invoke(HttpMethod::kPost, "/api/config/set",
            "{\"plugin_id\":\"plugin.alpha\",\"field\":\"enable\",\"key\":\"x\",\"value\":true}");
        assert(unknown_field.first == 400); // no generic arbitrary enable field
        const auto bad = invoke(HttpMethod::kPost, "/api/config/set", "{\"plugin_id\":\"plugin.alpha\"}");
        assert(bad.first == 400);
        const auto ordinary = invoke(HttpMethod::kPost, "/api/config/set",
            "{\"plugin_id\":\"plugin.alpha\",\"field\":\"config\",\"key\":\"parameter\",\"value\":42}");
        assert(ordinary.first == 200);
    }
    assert(!feature_control_http_route(HttpReq{HttpMethod::kGet, "/api/item/bag/0", 14, nullptr, 0, nullptr, 0}));
    std::ifstream persisted(file_path);
    const std::string bytes{std::istreambuf_iterator<char>(persisted), {}};
    assert(bytes.find("switch-name") == std::string::npos);
    production_feature_controller().shutdown();
    std::puts("production_feature_facade: PASS (ABI/prefix, explicit metadata, no constructor IO, resident token-free control, owner restrictions, guarded entry admission)");
}
