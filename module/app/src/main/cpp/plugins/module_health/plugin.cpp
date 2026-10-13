#include "plugin.h"

#include "contracts/host_sdk.h"
#include "contracts/module_health.h"

#include <mutex>

namespace qol::features::module_health {
namespace {

constexpr const char* kPluginId = "module_health";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kSchedulerUnavailable = "health frame scheduler unavailable";
constexpr const char* kSchedulerRegistrationFailed = "health frame task registration failed";
constexpr const char* kSchedulerRemovalFailed = "health frame task removal failed";
constexpr int32_t kHealthSampleIntervalFrames = 30;

std::mutex g_health_mutex;
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolHostModuleHealthFactsApi* g_host_facts = nullptr;
contracts::QolModuleHealthSnapshot g_latest_snapshot{};
uint64_t g_frame_task = 0;
bool g_snapshot_initialized = false;
bool g_sampling_enabled = false;
const char* g_scheduler_error = nullptr;

bool valid_snapshot(const contracts::QolModuleHealthSnapshot& snapshot) {
    return snapshot.struct_size >= sizeof(contracts::QolModuleHealthSnapshot) &&
           (snapshot.failed_mask & ~snapshot.known_mask) == 0 &&
           snapshot.main_menu_seen <= 1;
}

bool same_snapshot(const contracts::QolModuleHealthSnapshot& left,
                   const contracts::QolModuleHealthSnapshot& right) {
    return left.revision == right.revision && left.known_mask == right.known_mask &&
           left.failed_mask == right.failed_mask && left.main_menu_seen == right.main_menu_seen;
}

void cache_snapshot_locked(const contracts::QolModuleHealthSnapshot& snapshot) {
    if (!valid_snapshot(snapshot)) return;
    if (g_snapshot_initialized && snapshot.revision < g_latest_snapshot.revision) return;
    if (g_snapshot_initialized && same_snapshot(snapshot, g_latest_snapshot)) return;
    g_latest_snapshot = snapshot;
    g_snapshot_initialized = true;
}

QolResult read_health(void*, contracts::QolModuleHealthSnapshot* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    std::lock_guard<std::mutex> lock(g_health_mutex);
    if (!g_snapshot_initialized) return contracts::error(kQolErrNotReady, nullptr, 0);
    *out = g_latest_snapshot;
    return contracts::ok();
}

const contracts::QolHostModuleHealthFactsApi kModuleHealthApi{
    sizeof(contracts::QolHostModuleHealthFactsApi), nullptr, &read_health};

int32_t frame_tick(int64_t, void*) {
    std::lock_guard<std::mutex> lock(g_health_mutex);
    if (!g_sampling_enabled) {
        g_frame_task = 0;
        return 0;
    }
    if (g_host_facts == nullptr || g_host_facts->read == nullptr) return 0;

    contracts::QolModuleHealthSnapshot snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const QolResult result = g_host_facts->read(g_host_facts->context, &snapshot);
    if (contracts::succeeded(result)) cache_snapshot_locked(snapshot);
    return 1;
}

QolResult start_frame_task(const contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->scheduler.add == nullptr || sdk->scheduler.remove == nullptr) {
        std::lock_guard<std::mutex> lock(g_health_mutex);
        g_scheduler_error = kSchedulerUnavailable;
        return contracts::error(kQolErrNotReady, kSchedulerUnavailable,
                                sizeof(kSchedulerUnavailable) - 1);
    }
    {
        std::lock_guard<std::mutex> lock(g_health_mutex);
        if (g_frame_task != 0) {
            g_sampling_enabled = true;
            return contracts::ok();
        }
        g_sampling_enabled = true;
    }

    uint64_t task = 0;
    const QolResult result = sdk->scheduler.add(
        1, &frame_tick, nullptr, kHealthSampleIntervalFrames, 0, &task);
    if (!contracts::succeeded(result) || task == 0) {
        std::lock_guard<std::mutex> lock(g_health_mutex);
        g_sampling_enabled = false;
        g_scheduler_error = kSchedulerRegistrationFailed;
        return contracts::succeeded(result)
            ? contracts::error(kQolErrBlocked, kSchedulerRegistrationFailed,
                               sizeof(kSchedulerRegistrationFailed) - 1)
            : result;
    }

    {
        std::lock_guard<std::mutex> lock(g_health_mutex);
        g_frame_task = task;
        g_scheduler_error = nullptr;
    }
    return contracts::ok();
}

void stop_frame_task(const contracts::QolHostSdk* sdk) {
    uint64_t task = 0;
    {
        std::lock_guard<std::mutex> lock(g_health_mutex);
        g_sampling_enabled = false;
        task = g_frame_task;
    }
    if (task == 0) return;

    if (sdk == nullptr || sdk->scheduler.remove == nullptr) {
        std::lock_guard<std::mutex> lock(g_health_mutex);
        g_scheduler_error = kSchedulerUnavailable;
        return;
    }
    const QolResult result = sdk->scheduler.remove(task);
    if (!contracts::succeeded(result) && result.code != kQolErrNotFound) {
        std::lock_guard<std::mutex> lock(g_health_mutex);
        g_scheduler_error = kSchedulerRemovalFailed;
        return;
    }
    std::lock_guard<std::mutex> lock(g_health_mutex);
    if (g_frame_task == task) {
        g_frame_task = 0;
        g_scheduler_error = nullptr;
    }
}

QolResult feature_toggle(contracts::PluginContext&, uint8_t) {
    return contracts::ok();
}

int plugin_install(contracts::PluginContext& context) {
    if (context.sdk == nullptr) return 1;
    const auto* facts = contracts::module_health_if_available(context.sdk);
    if (facts == nullptr || facts->read == nullptr) return 1;

    contracts::QolModuleHealthSnapshot initial{};
    initial.struct_size = sizeof(initial);
    if (!contracts::succeeded(facts->read(facts->context, &initial)) ||
        !valid_snapshot(initial)) return 1;

    std::lock_guard<std::mutex> lock(g_health_mutex);
    if (g_frame_task != 0) return 1;
    g_sdk = context.sdk;
    g_host_facts = facts;
    g_latest_snapshot = initial;
    g_snapshot_initialized = true;
    g_sampling_enabled = false;
    g_scheduler_error = nullptr;
    return 0;
}

void plugin_on_enabled(contracts::PluginContext& context) {
    const QolResult result = start_frame_task(context.sdk);
    (void)result;
}

void plugin_on_disabled(contracts::PluginContext& context) {
    stop_frame_task(context.sdk);
}

void plugin_deactivate(contracts::PluginContext& context) {
    stop_frame_task(context.sdk);
    std::lock_guard<std::mutex> lock(g_health_mutex);
    g_host_facts = nullptr;
    g_sdk = nullptr;
    g_snapshot_initialized = false;
    if (g_frame_task == 0) g_scheduler_error = nullptr;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    std::lock_guard<std::mutex> lock(g_health_mutex);
    out.enabled = g_sdk != nullptr && g_host_facts != nullptr;
    out.error = g_scheduler_error;
}

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kModuleHealthCapabilityId, contracts::kModuleHealthCapabilityVersion,
     &kModuleHealthApi},
};

const contracts::FeatureDefinition kFeatures[] = {
    {"module_health.enabled", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "Module health", "", &feature_toggle, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    0,
    nullptr,
    1,
    kProvides,
    0,
    nullptr,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
    nullptr, 0,
    nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::module_health
