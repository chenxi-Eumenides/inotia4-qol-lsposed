// host/plugin_registry 能力时序与撤销级联 host 测试（ARCHITECTURE.md §3.4.1 / §3.4.4）：
// enabled 注册 / installed_disabled 不注册 / disable 先 revoking 后注销 / 级联
// （required → installed_disabled + reason + 资源释放；optional → degraded）/ 不自动恢复 /
// deactivate 撤销路径。资源安装原语与符号解析由 HostDeps 注入 fake。

#include "host/plugin_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/attribute_quality.h"
#include "contracts/commands.h"
#include "contracts/ownership.h"
#include "contracts/plugin.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"
#include "infra/capabilities.h"
#include "infra/config.h"
#include "infra/events.h"
#include "infra/status.h"

namespace qol::host {
void runtime_effects_publish_actual(const char*, bool) {}
void runtime_effects_unsubscribe_plugin(const char*) {}
}  // namespace qol::host

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::contracts::AttributeQualityApi;
using qol::contracts::AttributeQuality;
using qol::contracts::AttributeQualityItem;
using qol::contracts::AttributeQualityItemResult;
using qol::contracts::AttributeQualityOptions;
using qol::contracts::AttributeRange;
using qol::contracts::AttributeQualityTier;
using qol::contracts::CapabilityProvisionRef;
using qol::contracts::CapabilityRequirement;
using qol::contracts::PluginContext;
using qol::contracts::PluginDescriptor;
using qol::contracts::PluginStatusOut;
using qol::contracts::ResourceClaim;
using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourcePolicy;
using qol::contracts::ResourceRequirement;
using qol::contracts::ResourceState;
using qol::host::HostDeps;
using qol::host::install_all;
using qol::host::lifecycle_apply;
using qol::host::lifecycle_step;
using qol::host::plugin_registry_reset_for_test;
using qol::host::register_plugin;
using qol::host::sync_gates;
using qol::infra::FeatureState;
using qol::infra::FeatureStatus;
using qol::hooks::in_flight_enter;
using qol::hooks::in_flight_leave;

namespace {

constexpr const char* kProviderEntrySymbol = "F_TEST_VMA_CAP_PROVIDER";
constexpr const char* kRequiredEntrySymbol = "F_TEST_VMA_CAP_REQUIRED";
constexpr const char* kOptionalEntrySymbol = "F_TEST_VMA_CAP_OPTIONAL";

// ---------------------------------------------------------------------------
// fake 后端
// ---------------------------------------------------------------------------

int g_hook_calls = 0;
int g_unhook_calls = 0;
bool g_fail_optional_soft_acquire = false;

int32_t optional_soft_acquire(const char*) { return g_fail_optional_soft_acquire ? -1 : 0; }
void optional_soft_release(const char*) {}
const qol::contracts::OwnershipOps g_optional_soft_ownership_ops{
    &optional_soft_acquire, &optional_soft_release};

uintptr_t fake_resolve(const char* symbol, bool* out_followed) {
    if (out_followed != nullptr) *out_followed = false;
    if (symbol == nullptr) return 0;
    if (std::strcmp(symbol, kProviderEntrySymbol) == 0) return 0x1100;
    if (std::strcmp(symbol, kRequiredEntrySymbol) == 0) return 0x1200;
    if (std::strcmp(symbol, kOptionalEntrySymbol) == 0) return 0x1300;
    return 0x1400;
}

int fake_hook(void* target, void* replacement, void** backup) {
    (void)target;
    (void)replacement;
    ++g_hook_calls;
    if (backup != nullptr) *backup = reinterpret_cast<void*>(0x9000);
    return 0;
}

int fake_unhook(void* func) {
    (void)func;
    ++g_unhook_calls;
    return 0;
}

bool fake_gate(const char* key) {
    return key == nullptr ? true : qol::infra::config_get_bool(key, false);
}

HostDeps make_host_deps() { return HostDeps{&fake_resolve, &fake_hook, &fake_unhook}; }

// ---------------------------------------------------------------------------
// fake provider / consumers
// ---------------------------------------------------------------------------

int g_dummy_handler = 0;

bool g_provider_revoking = false;
int g_provider_install_calls = 0;
int g_provider_deactivate_calls = 0;
int g_provider_enabled_calls = 0;
int g_provider_disabled_calls = 0;

QolResult fake_query_item(qol::contracts::QolItemHandle, const AttributeQualityOptions*,
                          AttributeQualityItemResult*) {
    return g_provider_revoking ? qol::contracts::error(kQolErrNotReady, "revoking", 8)
                                : qol::contracts::ok();
}
QolResult fake_query_option_context(qol::contracts::QolItemHandle, int32_t, int32_t,
                                    const AttributeQualityOptions*, AttributeQualityItem*) {
    return qol::contracts::ok();
}
QolResult fake_evaluate(int32_t, AttributeRange, const AttributeQualityOptions*, AttributeQuality*) {
    return qol::contracts::ok();
}
const AttributeQualityApi g_provider_api{sizeof(AttributeQualityApi), &fake_query_item,
                                         &fake_query_option_context, &fake_evaluate};

int provider_install(PluginContext&) { ++g_provider_install_calls; return 0; }
void provider_deactivate(PluginContext&) { ++g_provider_deactivate_calls; }
void provider_on_enabled(PluginContext&) { ++g_provider_enabled_calls; g_provider_revoking = false; }
void provider_on_disabled(PluginContext&) { ++g_provider_disabled_calls; g_provider_revoking = true; }
void provider_fill_status(PluginStatusOut& out) { out.enabled = false; out.error = nullptr; }

int g_req_enabled_calls = 0;
int g_req_disabled_calls = 0;
int g_req_install_calls = 0;
int req_install(PluginContext&) { ++g_req_install_calls; return 0; }
int g_req_deactivate_calls = 0;
void req_deactivate(PluginContext&) { ++g_req_deactivate_calls; }
void req_on_enabled(PluginContext&) { ++g_req_enabled_calls; }
void req_on_disabled(PluginContext&) { ++g_req_disabled_calls; }
void req_fill_status(PluginStatusOut& out) { out.enabled = false; out.error = nullptr; }

int g_opt_enabled_calls = 0;
int opt_install(PluginContext&) { return 0; }
void opt_deactivate(PluginContext&) {}
void opt_on_enabled(PluginContext&) { ++g_opt_enabled_calls; }
void opt_on_disabled(PluginContext&) {}
void opt_fill_status(PluginStatusOut& out) { out.enabled = false; out.error = nullptr; }

ResourceClaim make_claim(const char* id, const char* owner, const char* gate) {
    ResourceClaim c{};
    c.id = id;
    c.symbol = std::strcmp(id, "hook:cap:prov") == 0 ? kProviderEntrySymbol
               : std::strcmp(id, "hook:cap:req") == 0 ? kRequiredEntrySymbol
                                                       : kOptionalEntrySymbol;
    c.mechanism = ResourceMechanism::kEntryHook;
    c.policy = ResourcePolicy::kPrivate;
    c.requirement = ResourceRequirement::kRequired;
    c.failure = ResourceFailure::kFailClosed;
    c.resumable = true;
    c.owner = owner;
    c.reason = "test";
    c.gate = gate;
    c.step_budget = 120;
    c.handler = &g_dummy_handler;
    return c;
}

ResourceClaim g_provider_resources[] = {
    make_claim("hook:cap:prov", "cap_provider", "cap.provider.on"),
};
ResourceClaim g_req_resources[] = {
    make_claim("hook:cap:req", "cap_consumer_req", "cap.consumer.on"),
};
ResourceClaim g_opt_resources[] = {
    make_claim("hook:cap:opt", "cap_consumer_opt", "cap.consumer.on"),
};
ResourceClaim make_optional_soft_gate_claim() {
    ResourceClaim claim = make_claim("hook:cap:opt-soft", "cap_consumer_opt",
                                     "cap.consumer.soft.on");
    claim.symbol = "ownership:cap.consumer.soft";
    claim.mechanism = ResourceMechanism::kOwnership;
    claim.handler = const_cast<qol::contracts::OwnershipOps*>(&g_optional_soft_ownership_ops);
    claim.requirement = ResourceRequirement::kOptional;
    claim.failure = ResourceFailure::kFailSoft;
    return claim;
}
ResourceClaim g_opt_gate_resources[] = {
    make_claim("hook:cap:opt", "cap_consumer_opt", "cap.consumer.on"),
    make_optional_soft_gate_claim(),
};

CapabilityProvisionRef g_provides[] = {
    {qol::contracts::kAttributeQualityCapabilityId,
     {qol::contracts::kAttributeQualityVersionMajor,
      qol::contracts::kAttributeQualityVersionMinor},
     &g_provider_api},
};
CapabilityRequirement g_requires[] = {
    {qol::contracts::kAttributeQualityCapabilityId,
     {qol::contracts::kAttributeQualityVersionMajor,
      qol::contracts::kAttributeQualityVersionMinor}},
};
CapabilityRequirement g_optional[] = {
    {qol::contracts::kAttributeQualityCapabilityId,
     {qol::contracts::kAttributeQualityVersionMajor,
      qol::contracts::kAttributeQualityVersionMinor}},
};

PluginDescriptor make_provider() {
    PluginDescriptor d{};
    d.id = "cap_provider";
    d.version = "0.1.0";
    d.requires_count = 0;
    d.requires = nullptr;
    d.optional_count = 0;
    d.optional_requires = nullptr;
    d.provides_count = 1;
    d.provides = g_provides;
    d.resources_count = 1;
    d.resources = g_provider_resources;
    d.jni_count = 0;
    d.jni = nullptr;
    d.install = &provider_install;
    d.deactivate = &provider_deactivate;
    d.on_enabled = &provider_on_enabled;
    d.on_disabled = &provider_on_disabled;
    d.fill_status = &provider_fill_status;
    d.on_resource_ready = nullptr;
    return d;
}

PluginDescriptor make_required_consumer() {
    PluginDescriptor d{};
    d.id = "cap_consumer_req";
    d.version = "0.1.0";
    d.requires_count = 1;
    d.requires = g_requires;
    d.optional_count = 0;
    d.optional_requires = nullptr;
    d.provides_count = 0;
    d.provides = nullptr;
    d.resources_count = 1;
    d.resources = g_req_resources;
    d.jni_count = 0;
    d.jni = nullptr;
    d.install = &req_install;
    d.deactivate = &req_deactivate;
    d.on_enabled = &req_on_enabled;
    d.on_disabled = &req_on_disabled;
    d.fill_status = &req_fill_status;
    d.on_resource_ready = nullptr;
    return d;
}

PluginDescriptor make_optional_consumer() {
    PluginDescriptor d = make_required_consumer();
    d.id = "cap_consumer_opt";
    d.requires_count = 0;
    d.requires = nullptr;
    d.optional_count = 1;
    d.optional_requires = g_optional;
    d.resources_count = 1;
    d.resources = g_opt_resources;
    d.install = &opt_install;
    d.deactivate = &opt_deactivate;
    d.on_enabled = &opt_on_enabled;
    d.on_disabled = &opt_on_disabled;
    d.fill_status = &opt_fill_status;
    return d;
}

void reset_all() {
    plugin_registry_reset_for_test();
    qol::hooks::registry_reset_for_test();
    qol::hooks::tx_reset_for_test();
    qol::infra::status_reset_for_test();
    qol::infra::capabilities_reset_for_test();
    qol::infra::config_reset_for_test();
    qol::infra::events_reset_for_test();
    qol::hooks::tx_set_deps(
        qol::hooks::TxDeps{&fake_resolve, &fake_hook, &fake_unhook, &fake_gate});
    g_hook_calls = 0;
    g_unhook_calls = 0;
    g_fail_optional_soft_acquire = false;
    g_provider_revoking = false;
    g_provider_install_calls = 0;
    g_provider_deactivate_calls = 0;
    g_provider_enabled_calls = 0;
    g_provider_disabled_calls = 0;
    g_req_enabled_calls = 0;
    g_req_disabled_calls = 0;
    g_req_install_calls = 0;
    g_req_deactivate_calls = 0;
    g_opt_enabled_calls = 0;
}

bool feature_state_is(const char* id, FeatureState expected) {
    FeatureStatus st{};
    return qol::infra::status_get(id, &st) && st.state == expected;
}

bool feature_status(const char* id, FeatureStatus* out) {
    return qol::infra::status_get(id, out);
}

bool feature_degraded(const char* id) {
    FeatureStatus status{};
    return feature_status(id, &status) && status.degraded;
}

const AttributeQualityApi* require_attribute_quality() {
    return static_cast<const AttributeQualityApi*>(qol::infra::require_capability(
        qol::contracts::kAttributeQualityCapabilityId,
        QolVersionRange{qol::contracts::kAttributeQualityVersionMajor,
                        qol::contracts::kAttributeQualityVersionMinor}));
}

int drive_lifecycle() {
    int steps = 0;
    while (lifecycle_step() > 0 && steps < 1000) ++steps;
    return steps;
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

void test_enabled_registers_capability() {
    reset_all();
    CHECK(qol::contracts::attribute_quality_api_size_valid(&g_provider_api));
    qol::infra::config_set_bool("cap.provider.on", true);
    CHECK(register_plugin(make_provider()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(qol::infra::capability_count() == 1);
    CHECK(require_attribute_quality() == &g_provider_api);
    CHECK(g_provider_enabled_calls == 1);
}

void test_installed_disabled_does_not_register() {
    reset_all();
    // gate 未设置默认 false → installed_disabled，不注册。
    CHECK(register_plugin(make_provider()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("cap_provider", FeatureState::kInstalledDisabled));
    CHECK(qol::infra::capability_count() == 0);
    CHECK(require_attribute_quality() == nullptr);
    CHECK(g_provider_disabled_calls == 1);
}

void test_missing_optional_capability_is_degraded() {
    reset_all();
    qol::infra::config_set_bool("cap.consumer.on", true);
    CHECK(register_plugin(make_optional_consumer()));
    CHECK(install_all(make_host_deps()) == 0);
    FeatureStatus status{};
    CHECK(feature_status("cap_consumer_opt", &status));
    CHECK(status.state == FeatureState::kEnabled);
    CHECK(status.degraded);
    CHECK(status.error != nullptr && std::strstr(status.error, "attribute.quality") != nullptr);
}

void test_disable_revokes_and_cascades() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", true);
    qol::infra::config_set_bool("cap.consumer.on", true);
    CHECK(register_plugin(make_provider()));
    CHECK(register_plugin(make_required_consumer()));
    CHECK(register_plugin(make_optional_consumer()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(qol::infra::capability_count() == 1);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kEnabled));
    CHECK(feature_state_is("cap_consumer_opt", FeatureState::kEnabled));
    FeatureStatus opt{};
    CHECK(feature_status("cap_consumer_opt", &opt));
    CHECK(!opt.degraded);
    CHECK(qol::hooks::state_of("hook:cap:prov") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:cap:opt") == ResourceState::kLive);

    const AttributeQualityApi* saved = require_attribute_quality();
    CHECK(saved != nullptr);
    CHECK(in_flight_enter("hook:cap:req"));
    g_unhook_calls = 0;

    // 关闭 provider 门禁：下一轮 sync_gates 走撤销路径（先 revoking，再级联，再注销）。
    qol::infra::config_set_bool("cap.provider.on", false);
    CHECK(sync_gates() >= 1);
    CHECK(lifecycle_step() > 0);  // HTTP route/handler drain precedes capability revoke.

    CHECK(qol::infra::capability_count() == 0);
    CHECK(require_attribute_quality() == nullptr);
    CHECK(g_provider_disabled_calls == 1);  // 提供者门禁已关
    CHECK(g_provider_revoking);
    // 撤销期新调用被提供者拒绝（能力仍在撤销事务内，指针尚未失效）。
    CHECK(saved->query_item(qol::contracts::QolItemHandle{0, 0}, nullptr, nullptr).ok != kQolOk);

    // required 消费者先进入 release 事务：在途未归零时保持 live/deactivating。
    FeatureStatus req{};
    CHECK(feature_status("cap_consumer_req", &req));
    CHECK(req.state == FeatureState::kDeactivating);
    CHECK(!req.enabled);
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kLive);
    CHECK(g_req_disabled_calls == 1);
    CHECK(g_unhook_calls == 0);

    CHECK(lifecycle_step() > 0);
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kDeactivating));
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kLive);
    CHECK(in_flight_leave("hook:cap:req"));
    CHECK(lifecycle_step() > 0);
    CHECK(feature_status("cap_consumer_req", &req));
    CHECK(req.state == FeatureState::kInstalledDisabled);
    CHECK(req.error != nullptr && std::strcmp(req.error, "cascade_stopped") == 0);
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kFree);
    CHECK(g_unhook_calls == 1);
    CHECK(g_req_deactivate_calls == 0);

    // optional 消费者：仅 degraded 标记，生命周期态与资源不变。
    FeatureStatus opt_after{};
    CHECK(feature_status("cap_consumer_opt", &opt_after));
    CHECK(opt_after.degraded);
    CHECK(opt_after.state == FeatureState::kEnabled);
    CHECK(qol::hooks::state_of("hook:cap:opt") == ResourceState::kLive);

    // Restoring the missing optional provider clears the cascade degraded source.
    qol::infra::config_set_bool("cap.provider.on", true);
    CHECK(sync_gates() >= 1);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(qol::infra::capability_count() == 1);
    FeatureStatus opt_after_restore{};
    CHECK(feature_status("cap_consumer_opt", &opt_after_restore));
    CHECK(opt_after_restore.state == FeatureState::kEnabled);
    CHECK(!opt_after_restore.degraded);
}

void test_provider_restore_preserves_gate_fail_soft_degraded() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", true);
    qol::infra::config_set_bool("cap.consumer.on", true);
    PluginDescriptor optional = make_optional_consumer();
    optional.resources = g_opt_gate_resources;
    optional.resources_count = sizeof(g_opt_gate_resources) / sizeof(g_opt_gate_resources[0]);
    CHECK(register_plugin(make_provider()));
    CHECK(register_plugin(optional));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(!feature_degraded("cap_consumer_opt"));

    qol::infra::config_set_bool("cap.provider.on", false);
    CHECK(sync_gates() >= 1);
    drive_lifecycle();
    CHECK(feature_degraded("cap_consumer_opt"));

    g_fail_optional_soft_acquire = true;
    qol::infra::config_set_bool("cap.consumer.soft.on", true);
    CHECK(sync_gates() >= 1);
    CHECK(feature_degraded("cap_consumer_opt"));
    CHECK(qol::hooks::state_of("hook:cap:opt-soft") == ResourceState::kFree);

    qol::infra::config_set_bool("cap.provider.on", true);
    CHECK(sync_gates() >= 1);
    drive_lifecycle();
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(qol::infra::capability_count() == 1);
    CHECK(feature_degraded("cap_consumer_opt"));

    g_fail_optional_soft_acquire = false;
    (void)sync_gates();
    CHECK(qol::hooks::state_of("hook:cap:opt-soft") == ResourceState::kLive);
    CHECK(!feature_degraded("cap_consumer_opt"));
}

void test_no_auto_recovery() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", true);
    qol::infra::config_set_bool("cap.consumer.on", true);
    CHECK(register_plugin(make_provider()));
    CHECK(register_plugin(make_required_consumer()));
    CHECK(install_all(make_host_deps()) == 0);

    qol::infra::config_set_bool("cap.provider.on", false);
    CHECK(sync_gates() >= 1);
    drive_lifecycle();
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kInstalledDisabled));

    // provider 恢复：重新注册能力，但被级联停用的消费者不自动恢复（§3.4.1 第 6 条）。
    qol::infra::config_set_bool("cap.provider.on", true);
    CHECK(sync_gates() >= 1);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(qol::infra::capability_count() == 1);

    FeatureStatus req{};
    CHECK(feature_status("cap_consumer_req", &req));
    CHECK(req.state == FeatureState::kInstalledDisabled);
    CHECK(req.error != nullptr && std::strcmp(req.error, "cascade_stopped") == 0);
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kFree);

    // 继续同步仍不恢复。
    CHECK(sync_gates() == 0);
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kInstalledDisabled));

    // 新一轮撤销事务必须重新建立 visited；旧的全局 visited 会漏掉这次回调。
    qol::infra::config_set_bool("cap.provider.on", false);
    CHECK(sync_gates() >= 1);
    drive_lifecycle();
    CHECK(g_req_disabled_calls == 2);
}

void test_cascade_stopped_consumer_explicit_activate_recovers() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", true);
    qol::infra::config_set_bool("cap.consumer.on", true);
    CHECK(register_plugin(make_provider()));
    CHECK(register_plugin(make_required_consumer()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kEnabled));

    qol::infra::config_set_bool("cap.provider.on", false);
    CHECK(sync_gates() >= 1);
    drive_lifecycle();

    FeatureStatus stopped{};
    CHECK(feature_status("cap_consumer_req", &stopped));
    CHECK(stopped.state == FeatureState::kInstalledDisabled);
    CHECK(stopped.error != nullptr && std::strcmp(stopped.error, "cascade_stopped") == 0);
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kFree);

    qol::infra::config_set_bool("cap.provider.on", true);
    CHECK(sync_gates() >= 1);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(require_attribute_quality() == &g_provider_api);

    CHECK(lifecycle_apply("cap_consumer_req", qol::contracts::kFeatureLifecycleActivate));
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kResolving));
    CHECK(lifecycle_step() > 0);
    drive_lifecycle();

    FeatureStatus recovered{};
    CHECK(feature_status("cap_consumer_req", &recovered));
    CHECK(recovered.state == FeatureState::kEnabled);
    CHECK(recovered.enabled);
    CHECK(recovered.error == nullptr);
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kLive);
    CHECK(g_req_install_calls == 2);
    CHECK(g_req_enabled_calls == 2);
}

void test_cascade_stopped_activate_missing_dependency_is_blocked() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", true);
    qol::infra::config_set_bool("cap.consumer.on", true);
    CHECK(register_plugin(make_provider()));
    CHECK(register_plugin(make_required_consumer()));
    CHECK(install_all(make_host_deps()) == 0);

    qol::infra::config_set_bool("cap.provider.on", false);
    CHECK(sync_gates() >= 1);
    drive_lifecycle();
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kInstalledDisabled));

    CHECK(!lifecycle_apply("cap_consumer_req", qol::contracts::kFeatureLifecycleActivate));
    FeatureStatus status{};
    CHECK(feature_status("cap_consumer_req", &status));
    CHECK(status.state == FeatureState::kBlocked);
    CHECK(status.error != nullptr && std::strstr(status.error, "attribute.quality") != nullptr);
    CHECK(lifecycle_step() == 0);
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kFree);
}

void test_required_consumer_before_provider_is_topologically_unblocked() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", true);
    qol::infra::config_set_bool("cap.consumer.on", true);
    // 注册顺序故意反转；DAG 拓扑应先安装/发布 provider，再自动解开 consumer。
    CHECK(register_plugin(make_required_consumer()));
    CHECK(register_plugin(make_provider()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kEnabled));
    CHECK(g_req_install_calls == 1);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(qol::infra::capability_count() == 1);
}

void test_blocked_consumer_recovers_after_provider_enable() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", false);
    qol::infra::config_set_bool("cap.consumer.on", true);
    // provider 后注册且初始门禁关闭：consumer 必须先停在 blocked。
    CHECK(register_plugin(make_required_consumer()));
    CHECK(register_plugin(make_provider()));
    CHECK(install_all(make_host_deps()) == 1);
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kBlocked));
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kFree);
    CHECK(g_req_enabled_calls == 0);

    // 同一轮先启用 provider、注册能力，再重评估 blocked consumer。
    qol::infra::config_set_bool("cap.provider.on", true);
    CHECK(sync_gates() >= 2);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:cap:prov") == ResourceState::kLive);
    CHECK(g_req_install_calls == 1);
    CHECK(g_req_enabled_calls == 1);
    CHECK(sync_gates() == 0);
    CHECK(g_req_enabled_calls == 1);
}

void test_blocked_lifecycle_activate_rechecks_required() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", false);
    qol::infra::config_set_bool("cap.consumer.on", true);
    CHECK(register_plugin(make_required_consumer()));
    CHECK(register_plugin(make_provider()));
    CHECK(install_all(make_host_deps()) == 1);
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kBlocked));

    // 能力仍缺失时，blocked → resolving 的重试必须回到 blocked 并返回 false。
    CHECK(!lifecycle_apply("cap_consumer_req", qol::contracts::kFeatureLifecycleActivate));
    FeatureStatus status{};
    CHECK(feature_status("cap_consumer_req", &status));
    CHECK(status.state == FeatureState::kBlocked);
    CHECK(status.error != nullptr && std::strstr(status.error, "attribute.quality") != nullptr);

    // 能力就绪后，blocked 的显式 activate 继续既有异步安装路径。
    CHECK(qol::infra::register_capability(
        qol::infra::CapabilityProvision{qol::contracts::kAttributeQualityCapabilityId,
                                        {qol::contracts::kAttributeQualityVersionMajor,
                                         qol::contracts::kAttributeQualityVersionMinor},
                                        &g_provider_api}));
    CHECK(lifecycle_apply("cap_consumer_req", qol::contracts::kFeatureLifecycleActivate));
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kResolving));
    drive_lifecycle();
    CHECK(feature_state_is("cap_consumer_req", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kLive);
    CHECK(g_req_enabled_calls == 1);
}

void test_missing_required_capability_is_diagnostic() {
    reset_all();
    qol::infra::config_set_bool("cap.consumer.on", true);
    CHECK(register_plugin(make_required_consumer()));
    CHECK(install_all(make_host_deps()) == 1);
    FeatureStatus status{};
    CHECK(feature_status("cap_consumer_req", &status));
    CHECK(status.state == FeatureState::kBlocked);
    CHECK(status.error != nullptr && std::strstr(status.error, "attribute.quality") != nullptr);
}

CapabilityProvisionRef g_cycle_a_provides[] = {
    {"cycle.a", {1, 0}, &g_provider_api},
};
CapabilityProvisionRef g_cycle_b_provides[] = {
    {"cycle.b", {1, 0}, &g_provider_api},
};
CapabilityRequirement g_cycle_a_requires[] = {
    {"cycle.b", {1, 0}},
};
CapabilityRequirement g_cycle_b_requires[] = {
    {"cycle.a", {1, 0}},
};

PluginDescriptor make_cycle_plugin(const char* id, CapabilityRequirement* requires,
                                   CapabilityProvisionRef* provides) {
    PluginDescriptor d{};
    d.id = id;
    d.version = "0.1.0";
    d.requires_count = 1;
    d.requires = requires;
    d.provides_count = 1;
    d.provides = provides;
    d.install = &req_install;
    d.deactivate = &req_deactivate;
    d.on_enabled = &req_on_enabled;
    d.on_disabled = &req_on_disabled;
    d.fill_status = &req_fill_status;
    return d;
}

void test_dependency_cycle_is_diagnostic() {
    reset_all();
    CHECK(register_plugin(make_cycle_plugin("cycle_a", g_cycle_a_requires, g_cycle_a_provides)));
    CHECK(register_plugin(make_cycle_plugin("cycle_b", g_cycle_b_requires, g_cycle_b_provides)));
    CHECK(install_all(make_host_deps()) == 2);
    FeatureStatus a{};
    FeatureStatus b{};
    CHECK(feature_status("cycle_a", &a));
    CHECK(feature_status("cycle_b", &b));
    CHECK(a.state == FeatureState::kFailed && a.error != nullptr &&
          std::strstr(a.error, "dependency cycle") != nullptr);
    CHECK(b.state == FeatureState::kFailed && b.error != nullptr &&
          std::strstr(b.error, "dependency cycle") != nullptr);
}

void test_deactivate_disallowed_is_rejected() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", true);
    PluginDescriptor descriptor = make_provider();
    descriptor.deactivate_allowed = false;
    CHECK(register_plugin(descriptor));
    CHECK(install_all(make_host_deps()) == 0);

    CHECK(!lifecycle_apply("cap_provider", qol::contracts::kFeatureLifecycleDeactivate));
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(g_provider_deactivate_calls == 0);
    CHECK(qol::hooks::state_of("hook:cap:prov") == ResourceState::kLive);
}

void test_deactivate_revokes_capability() {
    reset_all();
    qol::infra::config_set_bool("cap.provider.on", true);
    CHECK(register_plugin(make_provider()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(qol::infra::capability_count() == 1);

    CHECK(lifecycle_apply("cap_provider", qol::contracts::kFeatureLifecycleDeactivate));
    drive_lifecycle();

    CHECK(feature_state_is("cap_provider", FeatureState::kInactive));
    CHECK(qol::infra::capability_count() == 0);
    CHECK(require_attribute_quality() == nullptr);
    CHECK(g_provider_disabled_calls == 1);   // 撤销期关提供者门禁
    CHECK(g_provider_deactivate_calls == 1); // 终态插件自身清理
    CHECK(qol::hooks::state_of("hook:cap:prov") == ResourceState::kFree);
}

}  // namespace

int main() {
    test_enabled_registers_capability();
    test_installed_disabled_does_not_register();
    test_missing_optional_capability_is_degraded();
    test_disable_revokes_and_cascades();
    test_provider_restore_preserves_gate_fail_soft_degraded();
    test_no_auto_recovery();
    test_cascade_stopped_consumer_explicit_activate_recovers();
    test_cascade_stopped_activate_missing_dependency_is_blocked();
    test_required_consumer_before_provider_is_topologically_unblocked();
    test_blocked_consumer_recovers_after_provider_enable();
    test_blocked_lifecycle_activate_rechecks_required();
    test_missing_required_capability_is_diagnostic();
    test_dependency_cycle_is_diagnostic();
    test_deactivate_disallowed_is_rejected();
    test_deactivate_revokes_capability();
    std::printf("capability_lifecycle: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
