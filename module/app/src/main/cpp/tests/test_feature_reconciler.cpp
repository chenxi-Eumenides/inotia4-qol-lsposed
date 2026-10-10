#include "host/features/reconciler.h"
#include "host/item_count_facade.h"
#include "host/owner_identity.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "host/save_route.h"
#include "infra/store.h"
#include "contracts/commands.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"
#include "infra/capabilities.h"
#include "infra/status.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace qol;
using namespace qol::contracts;
using namespace qol::host;
using namespace qol::features::core_settings;

namespace {
struct Probe {
    int installs = 0;
    int enabled = 0;
    int disabled = 0;
    int cleanup = 0;
    int toggles = 0;
    int entries = 0;
    int subscriptions = 0;
    QolOwnerToken installed_token = 0;
};
std::map<std::string, Probe> probes;
std::vector<std::string> trace;
std::thread::id main_thread;
FeatureController* controller = nullptr;
FeatureReconciler* reconciler = nullptr;
bool fail_install = true;
bool fail_resource = true;
bool unsupported_ready = false;
bool fail_inverse_first = false;
std::set<uintptr_t> physical_hooks;
bool self_stop = false;
int hook_calls = 0;
int unhook_calls = 0;
int availability_calls = 0;
int handler = 0;
int provider_capability = 0;
int external_capability = 0;

// F1 fixture: a syntactically valid resident management channel. Only the settings plugin
// context must receive an SDK carrying it; every other context keeps feature_control=nullptr.
QolResult mgmt_read_catalog(void*, const QolFeatureCatalogPlugin**, uint32_t*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_read_desired(void*, const char*, uint8_t*, uint8_t*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_read_feature(void*, const char*, const char*, uint8_t*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_request_set_feature(void*, const char*, const char*, uint8_t, FeatureReceipt*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_request_desired(void*, const char*, uint8_t, FeatureReceipt*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_receipt(void*, uint64_t, FeatureReceipt*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_invoke_entry(void*, const char*, const char*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_read_config(void*, const char*, const char*, char*, uint32_t, uint32_t*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_request_set_config(void*, const char*, const char*, const char*, uint32_t, FeatureReceipt*) {
    return error(kQolErrNotReady, nullptr, 0);
}
QolResult mgmt_read_actual(void*, const char*, int32_t*, uint8_t*) {
    return error(kQolErrNotReady, nullptr, 0);
}
const QolFeatureControlApi kManagementControl{
    sizeof(QolFeatureControlApi), nullptr, &mgmt_read_catalog, &mgmt_read_desired,
    &mgmt_read_feature, &mgmt_request_set_feature, &mgmt_request_desired, &mgmt_receipt,
    &mgmt_invoke_entry, &mgmt_read_config, &mgmt_request_set_config, &mgmt_read_actual};

void assert_main() { assert(std::this_thread::get_id() == main_thread); }
FeatureReceipt await(FeatureReceipt receipt) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        assert(succeeded(controller->receipt(receipt.request_id, &receipt)));
        if (receipt.persistence != FeaturePersistence::kPending) return receipt;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "receipt timed out\n");
    std::abort();
}
FeatureReceipt set_feature(const char* plugin, const char* feature, uint8_t value) {
    FeatureReceipt receipt{};
    assert(succeeded(controller->request_set_feature(plugin, feature, value, &receipt)));
    receipt = await(receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted);
    return receipt;
}
FeatureReceipt set_desired(const char* plugin, uint8_t value) {
    FeatureReceipt receipt{};
    assert(succeeded(controller->request_desired(plugin, value, &receipt)));
    receipt = await(receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted);
    return receipt;
}
void frames(int count = 24) {
    for (int i = 0; i < count; ++i) reconciler->step();
}
infra::FeatureState actual(const char* id) {
    infra::FeatureStatus value{};
    assert(infra::status_get(id, &value));
    return value.state;
}
int install(PluginContext& ctx) {
    assert_main();
    auto& probe = probes[ctx.plugin_id];
    ++probe.installs;
    ++probe.subscriptions;
    probe.installed_token = ctx.owner_token;
    assert(ctx.owner_token != 0 && succeeded(owner_verify_token(ctx.owner_token)));
    trace.emplace_back(std::string("install:") + ctx.plugin_id);
    return std::strcmp(ctx.plugin_id, "test.failure") == 0 && fail_install ? 1 : 0;
}
void cleanup(PluginContext& ctx) {
    assert_main();
    auto& probe = probes[ctx.plugin_id];
    ++probe.cleanup;
    assert(probe.subscriptions == 1);
    --probe.subscriptions;
    trace.emplace_back(std::string("cleanup:") + ctx.plugin_id);
}
void enabled(PluginContext& ctx) { assert_main(); ++probes[ctx.plugin_id].enabled; }
void disabled(PluginContext& ctx) { assert_main(); ++probes[ctx.plugin_id].disabled; }
QolResult toggle(PluginContext& ctx, uint8_t) {
    assert_main();
    assert(feature_registry_active(ctx.plugin_id));
    ++probes[ctx.plugin_id].toggles;
    return ok();
}
QolResult entry(PluginContext& ctx) {
    assert_main();
    assert(feature_registry_active(ctx.plugin_id));
    ++probes[ctx.plugin_id].entries;
    if (self_stop) {
        self_stop = false;
        auto& api = controller->api();
        FeatureReceipt receipt{};
        assert(succeeded(api.request_set_feature(api.context, ctx.owner_token, ctx.plugin_id, "toggle", 0, &receipt)));
        await(receipt);
        assert(succeeded(api.request_set_feature(api.context, ctx.owner_token, ctx.plugin_id, "entry", 0, &receipt)));
        await(receipt);
        const auto start = std::chrono::steady_clock::now();
        // This callback still holds the reconciler's business/namespace lease.
        reconciler->step();
        reconciler->step();
        assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
        assert(actual(ctx.plugin_id) == infra::FeatureState::kDeactivating);
        assert(owner_token_of(ctx.plugin_id) == 0);
        assert(!succeeded(owner_verify_token(ctx.owner_token)));
    }
    return ok();
}
QolResult availability(PluginContext&, void*) { assert_main(); ++availability_calls; return ok(); }
struct HttpHold {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};
QolResult held_query(const HttpReq*, HttpRes* response, void* raw) {
    auto& held = *static_cast<HttpHold*>(raw);
    std::unique_lock<std::mutex> lock(held.mutex);
    held.entered = true;
    held.changed.notify_all();
    assert(held.changed.wait_for(lock, std::chrono::seconds(4), [&] { return held.release; }));
    response->status = 200;
    return ok();
}
const FeatureDefinition features[] = {
    {"toggle", 1, FeatureCategory::kHidden, FeatureType::kToggle, "toggle", "toggle", &toggle, nullptr},
    {"entry", 0, FeatureCategory::kTools, FeatureType::kEntry, "entry", "entry", nullptr, &entry}};
const CapabilityProvisionRef provides[] = {{"test.capability", {1, 0}, &provider_capability}};
const CapabilityRequirement requires[] = {{"test.capability", {1, 0}}};
const CapabilityRequirement missing[] = {{"test.external", {1, 0}}};
const CapabilityRequirement incompatible[] = {{"test.capability", {2, 0}}};
ResourceClaim claim(const char* id, const char* owner, const char* symbol) {
    ResourceClaim value{};
    value.id = id;
    value.owner = owner;
    value.symbol = symbol;
    value.mechanism = ResourceMechanism::kEntryHook;
    value.policy = ResourcePolicy::kPrivate;
    value.requirement = ResourceRequirement::kRequired;
    value.failure = ResourceFailure::kFailClosed;
    value.resumable = true;
    value.handler = &handler;
    value.step_budget = 120;
    return value;
}
const ResourceClaim provider_resource[] = {claim("feature:provider", "test.provider", "provider")};
const ResourceClaim failed_resources[] = {
    claim("feature:rollback:first", "test.resource_failure", "rollback-first"),
    claim("feature:rollback:second", "test.resource_failure", "rollback-second")};
const ResourceClaim retained_resources[] = {
    [] {
        auto value = claim("feature:retained:first", "test.retained", "retained-first");
        value.resumable = false;
        return value;
    }(),
    claim("feature:retained:second", "test.retained", "retained-second")};
const ResourceClaim unsupported_resource[] = {claim("feature:unsupported", "test.unsupported", "unsupported")};
// stack_limit 形状：单个 default-false feature + 单条 atomic 资源组；用于驱动 Host-owned
// 数量扩展模式策略（资源组 live → kApplied，请求未落地 → kNotReady，撤销 → kOff）。
const FeatureDefinition stack_limit_features[] = {
    {"stack_limit.limit_999", 0, FeatureCategory::kGameplay, FeatureType::kToggle, "999", "999",
     &toggle, nullptr}};
const ResourceClaim stack_limit_resource[] = {[] {
    auto value = claim("feature:stack_limit:atomic", "stack_limit", "stack-limit");
    value.atomic = true;
    return value;
}()};
PluginDescriptor descriptor(const char* id) {
    PluginDescriptor result{};
    result.id = id;
    result.version = "1";
    result.install = &install;
    result.deactivate = &cleanup;
    result.on_enabled = &enabled;
    result.on_disabled = &disabled;
    result.features = features;
    result.feature_count = 2;
    return result;
}
uintptr_t resolve(const char* symbol, bool* followed) {
    assert_main();
    if (followed) *followed = false;
    if (!symbol) return 0;
    if (std::strcmp(symbol, "unsupported") == 0) return unsupported_ready ? 0x4000 : 0;
    if (std::strcmp(symbol, "provider") == 0) return 0x1000;
    if (std::strcmp(symbol, "rollback-first") == 0) return 0x2000;
    if (std::strcmp(symbol, "retained-first") == 0) return 0x5000;
    if (std::strcmp(symbol, "retained-second") == 0) return 0x6000;
    if (std::strcmp(symbol, "stack-limit") == 0) return 0x7000;
    return 0x3000;
}
int hook(void* target, void*, void** backup) {
    assert_main();
    ++hook_calls;
    if (reinterpret_cast<uintptr_t>(target) == 0x3000 && fail_resource) return 1;
    if (reinterpret_cast<uintptr_t>(target) == 0x7000 && fail_resource) return 1;
    if (backup) *backup = reinterpret_cast<void*>(0xA000);
    physical_hooks.insert(reinterpret_cast<uintptr_t>(target));
    return 0;
}
int unhook(void* target) {
    assert_main();
    ++unhook_calls;
    const auto address = reinterpret_cast<uintptr_t>(target);
    if (fail_inverse_first && (address == 0x3000 || address == 0x6000)) return 1;
    physical_hooks.erase(address);
    return 0;
}
bool gate(const char*) { return true; }

void test_owner_poll() {
    int identity = 0;
    assert(owner_register(&identity, "test.identity"));
    QolOwnerToken token = 0;
    uint64_t generation = 0;
    assert(succeeded(owner_identity_api().query(&identity, &token, &generation)));
    OwnerNamespaceCall held;
    assert(succeeded(owner_namespace_call_begin(token, &held)));
    const auto started = std::chrono::steady_clock::now();
    assert(owner_suspend_begin(&identity));
    assert(!owner_suspend_poll(&identity));
    assert(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100));
    OwnerNamespaceCall rejected;
    assert(!succeeded(owner_namespace_call_begin(token, &rejected)));
    assert(!owner_resume(&identity));
    owner_namespace_call_end(&held);
    assert(owner_suspend_poll(&identity));
    assert(owner_resume(&identity));
    QolOwnerToken fresh = 0;
    uint64_t fresh_generation = 0;
    assert(succeeded(owner_identity_api().query(&identity, &fresh, &fresh_generation)));
    assert(fresh != token && fresh_generation > generation);
    assert(!succeeded(owner_verify_token(token)));
}

void hold_sidecar_lock(void* raw) {
    auto& held = *static_cast<HttpHold*>(raw);
    std::unique_lock<std::mutex> lock(held.mutex);
    held.entered = true;
    held.changed.notify_all();
    assert(held.changed.wait_for(lock, std::chrono::seconds(4), [&] { return held.release; }));
}
void wait_held(HttpHold& held) {
    std::unique_lock<std::mutex> lock(held.mutex);
    assert(held.changed.wait_for(lock, std::chrono::seconds(3), [&] { return held.entered; }));
}
void release_held(HttpHold& held) {
    { std::lock_guard<std::mutex> lock(held.mutex); held.release = true; }
    held.changed.notify_all();
}
void test_sidecar_pending() {
    // Same blocking + try callback pair installed by the real HostSdk Store APIs.
    owner_store_lifecycle_handlers(&infra::store::owner_stopped, &infra::store::owner_rebound);
    owner_store_lifecycle_try_handlers(&infra::store::owner_stopped_try, &infra::store::owner_rebound_try);
    const auto old = owner_token_of("test.solo");
    const auto cleanup_before = probes["test.solo"].cleanup;
    HttpHold stopped_lock;
    std::thread holder([&] { infra::store::with_store_lock_for_test(&hold_sidecar_lock, &stopped_lock); });
    wait_held(stopped_lock);
    set_desired("test.solo", 0);
    const auto start = std::chrono::steady_clock::now();
    frames();
    assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
    assert(owner_token_of("test.solo") == 0 && !succeeded(owner_verify_token(old)));
    assert(probes["test.solo"].cleanup == cleanup_before);
    assert(actual("test.solo") == infra::FeatureState::kDeactivating);
    release_held(stopped_lock);
    holder.join();
    frames();
    assert(actual("test.solo") == infra::FeatureState::kInactive);

    const auto installs_before = probes["test.solo"].installs;
    HttpHold rebound_lock;
    std::thread rebound_holder([&] { infra::store::with_store_lock_for_test(&hold_sidecar_lock, &rebound_lock); });
    wait_held(rebound_lock);
    set_desired("test.solo", 1);
    const auto rebound_start = std::chrono::steady_clock::now();
    frames();
    assert(std::chrono::steady_clock::now() - rebound_start < std::chrono::milliseconds(100));
    assert(owner_token_of("test.solo") == 0);
    assert(probes["test.solo"].installs == installs_before);
    assert(actual("test.solo") == infra::FeatureState::kInactive);
    release_held(rebound_lock);
    rebound_holder.join();
    frames();
    assert(feature_registry_active("test.solo") && owner_token_of("test.solo") != old);
    assert(probes["test.solo"].installs == installs_before + 1);
    std::puts("sidecar_owner_pending: pass (real slot_operation/Store mutex contention; frame returns; cleanup/token publication/install deferred)");
}

struct SaveHold {
    HttpHold held;
    std::atomic<bool> busy{true};
};
QolResult save_native(void* raw, const QolSaveNativeCall*, QolSaveDomainStatus*) {
    hold_sidecar_lock(&static_cast<SaveHold*>(raw)->held);
    return ok();
}
QolResult save_render(void*, uint64_t, const QolSaveAdapterState*, QolSaveDomainStatus*) { return ok(); }
QolResult save_prepare(void* raw) {
    return static_cast<SaveHold*>(raw)->busy.load() ? error(kQolErrBusy, nullptr, 0) : ok();
}
void test_save_busy_and_inflight() {
    const auto token = owner_token_of("test.solo");
    SaveHold hold;
    QolSaveOwnerCallbacks callbacks{{sizeof(QolSaveOwnerCallbacks), 1, 0}, &hold,
                                    &save_native, &save_render, &save_prepare};
    uint64_t binding = 0;
    const auto& route = save_route::save_route_api();
    assert(succeeded(route.bind(route.host_ctx, token, &callbacks, &binding)));
    const auto cleanup_before = probes["test.solo"].cleanup;
    set_desired("test.solo", 0);
    frames();
    assert(actual("test.solo") == infra::FeatureState::kEnabled && feature_registry_active("test.solo"));
    assert(owner_token_of("test.solo") == token && succeeded(owner_verify_token(token)));
    assert(probes["test.solo"].cleanup == cleanup_before);
    QolSaveNativeCall call{};
    call.size = sizeof(call);
    std::thread callback([&] {
        QolSaveDomainStatus status{};
        assert(succeeded(save_route::dispatch_native_call(token, &call, &status)));
    });
    wait_held(hold.held);
    hold.busy.store(false);
    const auto start = std::chrono::steady_clock::now();
    frames();
    assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
    assert(actual("test.solo") == infra::FeatureState::kDeactivating && owner_token_of("test.solo") == 0);
    assert(probes["test.solo"].cleanup == cleanup_before);
    release_held(hold.held);
    callback.join();
    frames();
    assert(actual("test.solo") == infra::FeatureState::kInactive);
    assert(probes["test.solo"].cleanup == cleanup_before + 1);
    set_desired("test.solo", 1);
    frames();
    assert(feature_registry_active("test.solo") && owner_token_of("test.solo") != token);
    std::puts("save_route_stop_pending: pass (Busy retains intent and token; revoked binding in-flight drains before cleanup)");
}
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    main_thread = std::this_thread::get_id();
    test_owner_poll();
    plugin_registry_reset_for_test();
    hooks::registry_reset_for_test();
    hooks::tx_reset_for_test();
    hooks::tx_set_deps(hooks::TxDeps{&resolve, &hook, &unhook, &gate});
    auto provider = descriptor("test.provider");
    provider.provides = provides;
    provider.provides_count = 1;
    provider.resources = provider_resource;
    provider.resources_count = 1;
    auto consumer = descriptor("test.consumer");
    consumer.requires = requires;
    consumer.requires_count = 1;
    auto solo = descriptor("test.solo");
    auto failure = descriptor("test.failure");
    auto resource_failure = descriptor("test.resource_failure");
    resource_failure.resources = failed_resources;
    resource_failure.resources_count = 2;
    auto retained = descriptor("test.retained");
    retained.resources = retained_resources;
    retained.resources_count = 2;
    auto absent = descriptor("test.missing");
    absent.requires = missing;
    absent.requires_count = 1;
    auto version = descriptor("test.version");
    version.requires = incompatible;
    version.requires_count = 1;
    auto unsupported = descriptor("test.unsupported");
    unsupported.resources = unsupported_resource;
    unsupported.resources_count = 1;
    auto stack_limit = descriptor("stack_limit");
    stack_limit.features = stack_limit_features;
    stack_limit.feature_count = 1;
    stack_limit.resources = stack_limit_resource;
    stack_limit.resources_count = 1;
    auto settings = descriptor("settings");
    FeatureDirectory directory;
    for (const auto& plugin : {consumer, solo, failure, provider, resource_failure, retained, absent, version, unsupported, stack_limit, settings}) {
        assert(register_plugin(plugin));
        assert(directory.add(plugin).ok());
    }
    assert(bind_plugin_store(solo, "module-saves"));
    assert(bind_plugin_save_route(solo));
    // F1: Host selects the management SDK copy; only the settings context receives it.
    contracts::QolHostSdk management_sdk{};
    management_sdk.abi = {sizeof(contracts::QolHostSdk), kHostSdkMajor, kHostSdkMinor};
    management_sdk.feature_control = &kManagementControl;
    bind_management_sdk(management_sdk);
    // A capability not owned by a registered provider must not satisfy the new DAG.
    assert(infra::register_capability({"test.external", {1, 0}, &external_capability}));
    std::string path = std::string(argv[1]) + "/feature-reconciler-XXXXXX";
    assert(::mkdtemp(path.data()));
    CoreSettingsConfigService service;
    FeatureController control(service);
    controller = &control;
    FeatureReconciler reconcile(control);
    reconciler = &reconcile;
    assert(control.initialize(directory, path, &FeatureReconciler::pending_desired, &reconcile).ok());
    assert(reconcile.bind({&resolve, &hook, &unhook}));
    reconcile.step(); // Starts the provider's shared install transaction, but does not advance it.
    const auto hooks_before_old_step = hook_calls;
    for (int i = 0; i < 10; ++i) assert(lifecycle_step() == 0);
    assert(hook_calls == hooks_before_old_step && probes["test.provider"].enabled == 0);
    assert(infra::require_capability("test.capability", {1, 0}) == nullptr);
    assert(sync_gates() == 0 && install_all({&resolve, &hook, &unhook}) == -1);
    frames();
    assert(feature_registry_active("test.provider") && feature_registry_active("test.consumer"));
    assert(feature_registry_active("test.solo"));
    assert(probes["test.provider"].installs == 1 && probes["test.consumer"].installs == 1);

    // F1: only settings receives the management SDK copy; ordinary contexts keep it null.
    contracts::PluginContext settings_ctx{};
    assert(feature_registry_context("settings", &settings_ctx));
    assert(settings_ctx.sdk != nullptr && settings_ctx.sdk->feature_control == &kManagementControl);
    assert(contracts::feature_control_if_available(settings_ctx.sdk) == &kManagementControl);
    contracts::PluginContext solo_ctx{};
    assert(feature_registry_context("test.solo", &solo_ctx));
    assert(solo_ctx.sdk != nullptr && solo_ctx.sdk->feature_control == nullptr);
    assert(contracts::feature_control_if_available(solo_ctx.sdk) == nullptr);

    auto provider_position = std::find(trace.begin(), trace.end(), "install:test.provider");
    auto consumer_position = std::find(trace.begin(), trace.end(), "install:test.consumer");
    assert(provider_position < consumer_position);
    assert(probes["test.failure"].installs >= 1 &&
           probes["test.failure"].cleanup == probes["test.failure"].installs);
    assert(actual("test.failure") == infra::FeatureState::kFailed);
    assert(actual("test.resource_failure") == infra::FeatureState::kFailed);
    assert(probes["test.resource_failure"].cleanup >= 1);
    assert(hooks::state_of("feature:rollback:first") == ResourceState::kFree && unhook_calls >= 1);
    assert(probes["test.missing"].installs == 0 && probes["test.version"].installs == 0);
    assert(actual("test.missing") == infra::FeatureState::kBlocked);
    assert(actual("test.unsupported") == infra::FeatureState::kUnsupported);
    assert(probes["test.unsupported"].installs == 0);
    const auto failing_installs = probes["test.failure"].installs;
    const auto resource_installs = probes["test.resource_failure"].installs;
    frames(100);
    assert(probes["test.failure"].installs == failing_installs);
    assert(probes["test.resource_failure"].installs == resource_installs);
    reconcile.notify_dependency_changed();
    frames();
    assert(probes["test.failure"].installs == failing_installs + 1);
    assert(probes["test.resource_failure"].installs == resource_installs + 1);
    frames(100);
    assert(probes["test.failure"].installs == failing_installs + 1);
    fail_install = false;
    fail_resource = false;
    unsupported_ready = true;
    reconcile.notify_dependency_changed();
    frames();
    assert(feature_registry_active("test.failure") && feature_registry_active("test.resource_failure"));
    assert(feature_registry_active("test.unsupported") && probes["test.unsupported"].installs == 1);

    // Provider all-false tears down the zero-resource consumer first, without rewriting desired.
    const auto old_consumer = owner_token_of("test.consumer");
    const auto provider_disabled_before = probes["test.provider"].disabled;
    assert(hooks::in_flight_enter("feature:provider"));
    trace.clear();
    set_feature("test.provider", "toggle", 0);
    frames();
    assert(actual("test.provider") == infra::FeatureState::kDeactivating);
    assert(hooks::state_of("feature:provider") == ResourceState::kDisabled);
    assert(probes["test.provider"].disabled == provider_disabled_before);
    assert(probes["test.provider"].subscriptions == 1);
    assert(actual("test.consumer") == infra::FeatureState::kInactive);
    assert(hooks::in_flight_leave("feature:provider"));
    frames();
    assert(actual("test.provider") == infra::FeatureState::kInactive);
    assert(actual("test.consumer") == infra::FeatureState::kInactive);
    assert(probes["test.consumer"].subscriptions == 0 && probes["test.provider"].subscriptions == 0);
    assert(std::find(trace.begin(), trace.end(), "cleanup:test.consumer") <
           std::find(trace.begin(), trace.end(), "cleanup:test.provider"));
    std::vector<PluginDesiredSnapshot> desired;
    assert(control.desired(&desired).ok());
    for (const auto& snapshot : desired) assert(snapshot.desired_enable);
    set_feature("test.provider", "toggle", 1);
    frames();
    assert(feature_registry_active("test.provider") && feature_registry_active("test.consumer"));
    assert(owner_token_of("test.consumer") != old_consumer && !succeeded(owner_verify_token(old_consumer)));

    // Dependency recovery is automatic through the production frame step: the capability
    // becoming available again is a false→true edge that retries the blocked consumer once,
    // with no config write and no explicit notify_dependency_changed().
    infra::unregister_capability("test.capability");
    frames();
    assert(actual("test.consumer") == infra::FeatureState::kInactive);
    assert(feature_registry_active("test.provider"));
    assert(infra::register_capability({"test.capability", {1, 0}, &provider_capability}));
    frames();
    assert(feature_registry_active("test.consumer"));

    test_sidecar_pending();
    test_save_busy_and_inflight();

    // Actual/inactive guards apply to both queued entry dispatch and availability probes.
    set_feature("test.solo", "entry", 1);
    frames();
    assert(succeeded(control.invoke_entry("test.solo", "entry")));
    set_desired("test.solo", 0);
    frames();
    const auto entries = probes["test.solo"].entries;
    const auto toggles = probes["test.solo"].toggles;
    assert(succeeded(control.invoke_entry("test.solo", "entry")));
    assert(reconcile.probe_availability("test.solo", "entry", &availability, nullptr).code == kQolErrNotReady);
    frames();
    assert(probes["test.solo"].entries == entries && availability_calls == 0);
    assert(probes["test.solo"].toggles == toggles);
    set_feature("test.solo", "toggle", 0);
    set_feature("test.solo", "entry", 0);
    set_feature("test.solo", "toggle", 1);
    frames();
    assert(actual("test.solo") == infra::FeatureState::kInactive); // desired=false stays off.
    set_feature("test.solo", "toggle", 0);
    set_desired("test.solo", 1);
    frames();
    assert(actual("test.solo") == infra::FeatureState::kInactive); // Explicit on + all false stays suspended.
    set_feature("test.solo", "toggle", 1);
    set_feature("test.solo", "entry", 1);
    frames();
    assert(feature_registry_active("test.solo"));
    assert(succeeded(reconcile.probe_availability("test.solo", "entry", &availability, nullptr)));
    assert(availability_calls == 1);
    self_stop = true;
    const auto self_token = owner_token_of("test.solo");
    assert(succeeded(control.invoke_entry("test.solo", "entry")));
    frames();
    assert(actual("test.solo") == infra::FeatureState::kInactive && probes["test.solo"].subscriptions == 0);
    assert(!succeeded(owner_verify_token(self_token)));
    set_feature("test.solo", "toggle", 1);
    frames();
    assert(feature_registry_active("test.solo") && owner_token_of("test.solo") != self_token);

    // A newer durable revision during drain cannot resurrect the old token or skip cleanup.
    const auto draining_token = owner_token_of("test.solo");
    OwnerNamespaceCall held;
    assert(succeeded(owner_namespace_call_begin(draining_token, &held)));
    set_feature("test.solo", "toggle", 0);
    frames(2);
    assert(actual("test.solo") == infra::FeatureState::kDeactivating);
    assert(owner_token_of("test.solo") == 0);
    const auto during_drain = set_feature("test.solo", "toggle", 1);
    frames();
    assert(actual("test.solo") == infra::FeatureState::kDeactivating);
    assert(reconcile.applied_revision() == during_drain.revision);
    owner_namespace_call_end(&held);
    frames();
    assert(feature_registry_active("test.solo") && owner_token_of("test.solo") != draining_token);
    assert(!succeeded(owner_verify_token(draining_token)));

    const auto resource_cleanup_before = probes["test.resource_failure"].cleanup;
    fail_inverse_first = true;
    set_feature("test.resource_failure", "toggle", 0);
    frames();
    assert(actual("test.resource_failure") == infra::FeatureState::kFailed);
    assert(probes["test.resource_failure"].cleanup == resource_cleanup_before + 1);
    assert(probes["test.resource_failure"].subscriptions == 0);
    assert(hooks::state_of("feature:rollback:first") == ResourceState::kFree);
    assert(physical_hooks.count(0x2000) == 0);
    assert(hooks::state_of("feature:rollback:second") == ResourceState::kTombstone);
    assert(hooks::isolated_get("feature:rollback:second"));
    assert(!feature_registry_active("test.resource_failure"));
    fail_inverse_first = false;
    std::puts("release_failure_sweep: pass (inverse first-step failure; remaining resource physically released; failed resource tombstone/isolated; plugin body retired)");

    assert(feature_registry_active("test.retained"));
    assert(physical_hooks.count(0x5000) && physical_hooks.count(0x6000));
    const auto unhooks_before_retained = unhook_calls;
    fail_inverse_first = true;
    set_feature("test.retained", "toggle", 0);
    frames();
    assert(actual("test.retained") == infra::FeatureState::kFailed);
    assert(unhook_calls == unhooks_before_retained + 1);
    assert(physical_hooks.count(0x5000) && physical_hooks.count(0x6000));
    assert(hooks::state_of("feature:retained:first") == ResourceState::kTombstone);
    assert(hooks::isolated_get("feature:retained:first"));
    assert(hooks::state_of("feature:retained:second") == ResourceState::kTombstone);
    assert(hooks::isolated_get("feature:retained:second"));
    assert(probes["test.retained"].subscriptions == 0);
    fail_inverse_first = false;
    std::puts("nonresumable_failure_sweep: pass (unhook skipped; physical hook retained and isolated)");

    // HTTP admission is revoked immediately, but frame teardown polls the existing lease.
    const HttpRoute route{HttpMethod::kGet, "/api/test/drain", "test.solo", 0, 1};
    assert(succeeded(http_registry_api().register_route(&route)));
    const auto dispatch = http_dispatch_api();
    HttpHold http_hold;
    assert(succeeded(dispatch.register_query(HttpMethod::kGet, route.path, route.owner, &held_query, &http_hold)));
    const HttpReq request{HttpMethod::kGet, route.path, static_cast<uint32_t>(std::strlen(route.path)), nullptr, 0, nullptr, 0, nullptr, 0};
    std::thread http_thread([&] {
        uint8_t buffer[128]{};
        HttpRes response{buffer, sizeof(buffer), 0, 0};
        assert(succeeded(dispatch.dispatch(&request, &response)) && response.status == 200);
    });
    {
        std::unique_lock<std::mutex> lock(http_hold.mutex);
        assert(http_hold.changed.wait_for(lock, std::chrono::seconds(3), [&] { return http_hold.entered; }));
    }
    set_desired("test.solo", 0);
    const auto http_start = std::chrono::steady_clock::now();
    frames(2);
    assert(std::chrono::steady_clock::now() - http_start < std::chrono::milliseconds(100));
    assert(actual("test.solo") == infra::FeatureState::kDeactivating);
    assert(dispatch.register_query(HttpMethod::kGet, route.path, route.owner, &held_query, &http_hold).code == kQolErrConflict);
    uint8_t denied_buffer[128]{};
    HttpRes denied{denied_buffer, sizeof(denied_buffer), 0, 0};
    assert(!succeeded(dispatch.dispatch(&request, &denied)) && denied.status == 503);
    {
        std::lock_guard<std::mutex> lock(http_hold.mutex);
        http_hold.release = true;
    }
    http_hold.changed.notify_all();
    http_thread.join();
    frames();
    assert(actual("test.solo") == infra::FeatureState::kInactive);
    set_desired("test.solo", 1);
    frames();
    assert(feature_registry_active("test.solo"));
    assert(succeeded(dispatch.register_query(HttpMethod::kGet, route.path, route.owner, &held_query, &http_hold)));

    // Block the worker's file open: frame/control reads must use the published projection,
    // not contend on the Store mutex that is held throughout persistence.
    const auto fifo = path + "/config.json.tmp";
    assert(::mkfifo(fifo.c_str(), 0600) == 0);
    FeatureReceipt blocked{};
    assert(succeeded(control.request_desired("test.solo", 0, &blocked)));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto frame_start = std::chrono::steady_clock::now();
    frames();
    uint8_t feature_value = 0;
    assert(succeeded(control.read_feature("test.solo", "toggle", &feature_value)) && feature_value);
    assert(control.desired(&desired).ok());
    assert(std::chrono::steady_clock::now() - frame_start < std::chrono::milliseconds(100));
    assert(feature_registry_active("test.solo"));
    const int fifo_reader = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
    assert(fifo_reader >= 0);
    assert(await(blocked).persistence == FeaturePersistence::kFailed); // fsync on FIFO is invalid.
    assert(::close(fifo_reader) == 0);

    // Existing lifecycle command seam is desired admission only; write failure never stops actual.
    assert(::rename((path + "/config.json").c_str(), (path + "/saved.json").c_str()) == 0);
    assert(::mkdir((path + "/config.json").c_str(), 0700) == 0);
    FeatureReceipt failed{};
    assert(succeeded(control.request_desired("test.solo", 0, &failed)));
    failed = await(failed);
    assert(failed.persistence == FeaturePersistence::kFailed);
    frames();
    assert(feature_registry_active("test.solo"));
    assert(::rmdir((path + "/config.json").c_str()) == 0);
    assert(::rename((path + "/saved.json").c_str(), (path + "/config.json").c_str()) == 0);
    const auto before_off = service.revision();
    assert(lifecycle_apply("test.solo", kFeatureLifecycleDeactivate));
    for (int i = 0; i < 1000 && service.revision() == before_off; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(service.revision() > before_off);
    // Wait until its ordered pending notification is delivered before advancing frames.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    frames();
    assert(actual("test.solo") == infra::FeatureState::kInactive);
    assert(probes["test.solo"].subscriptions == 0);

    // Host-owned 数量扩展模式策略：stack_limit 资源组 live/revoke 驱动，失败保持 NotReady。
    fail_resource = false;
    uint8_t extended = 0;
    assert(qol::host::host_item_count_api().read_extended_mode(&extended).ok == 0 && extended == 0);
    assert(!qol::game::item_count_active());
    set_feature("stack_limit", "stack_limit.limit_999", 1);
    frames();
    assert(feature_registry_active("stack_limit"));
    extended = 0;
    assert(qol::host::host_item_count_api().read_extended_mode(&extended).ok == 0 && extended == 1);
    assert(qol::game::item_count_active());
    set_feature("stack_limit", "stack_limit.limit_999", 0);
    frames();
    assert(!feature_registry_active("stack_limit"));
    extended = 9;
    assert(qol::host::host_item_count_api().read_extended_mode(&extended).ok == 0 && extended == 0);
    assert(!qol::game::item_count_active());

    // 请求开启但资源组安装失败：保持 NotReady，不伪装关闭。
    fail_resource = true;
    set_feature("stack_limit", "stack_limit.limit_999", 1);
    frames();
    assert(!feature_registry_active("stack_limit"));
    extended = 9;
    assert(qol::host::host_item_count_api().read_extended_mode(&extended).code == kQolErrNotReady);
    assert(extended == 9);
    assert(!qol::game::item_count_active());
    fail_resource = false;
    set_feature("stack_limit", "stack_limit.limit_999", 0);
    frames();
    extended = 9;
    assert(qol::host::host_item_count_api().read_extended_mode(&extended).ok == 0 && extended == 0);
    std::puts("item_count_policy: pass (resource group live/revoke drives Host-owned extended mode; failure stays NotReady)");

    std::puts("feature_reconciler_tests: pass (topology/cascade/recovery, self-stop namespace/HTTP nonblocking drain/new revision, new tokens, explicit desired/all-false, failure once per batch/resource rollback, inactive entry/probe, write failure/frame not waiting on I/O)");
}
