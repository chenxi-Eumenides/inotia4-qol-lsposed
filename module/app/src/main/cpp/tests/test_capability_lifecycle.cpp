// host/plugin_registry 能力时序与撤销级联 host 测试（ARCHITECTURE.md §3.4.1 / §3.4.4）：
// enabled 注册 / installed_disabled 不注册 / disable 先 revoking 后注销 / 级联
// （required → installed_disabled + reason + 资源释放；optional → degraded）/ 不自动恢复 /
// deactivate 撤销路径。资源安装原语与符号解析由 HostDeps 注入 fake。

#include "host/plugin_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/attribute_range.h"
#include "contracts/commands.h"
#include "contracts/plugin.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"
#include "infra/capabilities.h"
#include "infra/config.h"
#include "infra/events.h"
#include "infra/status.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::contracts::AttrRangeApi;
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

namespace {

// ---------------------------------------------------------------------------
// fake 后端
// ---------------------------------------------------------------------------

int g_hook_calls = 0;
int g_unhook_calls = 0;

uintptr_t fake_resolve(const char* symbol, bool* out_followed) {
    (void)symbol;
    if (out_followed != nullptr) *out_followed = false;
    return 0x1000;
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

bool fake_probe_jewel_range(int type, void* item, int* out_min, int* out_max) {
    (void)type;
    (void)item;
    (void)out_min;
    (void)out_max;
    return !g_provider_revoking;  // 撤销期拒绝新调用
}
const AttrRangeApi g_provider_api{&fake_probe_jewel_range};

int provider_install(PluginContext&) { ++g_provider_install_calls; return 0; }
void provider_deactivate(PluginContext&) { ++g_provider_deactivate_calls; }
void provider_on_enabled(PluginContext&) { ++g_provider_enabled_calls; g_provider_revoking = false; }
void provider_on_disabled(PluginContext&) { ++g_provider_disabled_calls; g_provider_revoking = true; }
void provider_fill_status(PluginStatusOut& out) { out.enabled = false; out.error = nullptr; }

int g_req_enabled_calls = 0;
int g_req_disabled_calls = 0;
int g_req_install_calls = 0;
int req_install(PluginContext&) { ++g_req_install_calls; return 0; }
void req_deactivate(PluginContext&) {}
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
    c.symbol = "F_TEST_VMA";
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

CapabilityProvisionRef g_provides[] = {
    {qol::contracts::kAttrRangeCapabilityId, {1, 0}, &g_provider_api},
};
CapabilityRequirement g_requires[] = {
    {qol::contracts::kAttrRangeCapabilityId, {1, 0}},
};
CapabilityRequirement g_optional[] = {
    {qol::contracts::kAttrRangeCapabilityId, {1, 0}},
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
    g_provider_revoking = false;
    g_provider_install_calls = 0;
    g_provider_deactivate_calls = 0;
    g_provider_enabled_calls = 0;
    g_provider_disabled_calls = 0;
    g_req_enabled_calls = 0;
    g_req_disabled_calls = 0;
    g_req_install_calls = 0;
    g_opt_enabled_calls = 0;
}

bool feature_state_is(const char* id, FeatureState expected) {
    FeatureStatus st{};
    return qol::infra::status_get(id, &st) && st.state == expected;
}

bool feature_status(const char* id, FeatureStatus* out) {
    return qol::infra::status_get(id, out);
}

const AttrRangeApi* require_attr_range() {
    return static_cast<const AttrRangeApi*>(qol::infra::require_capability(
        qol::contracts::kAttrRangeCapabilityId,
        QolVersionRange{qol::contracts::kAttrRangeVersionMajor,
                        qol::contracts::kAttrRangeVersionMinor}));
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
    qol::infra::config_set_bool("cap.provider.on", true);
    CHECK(register_plugin(make_provider()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("cap_provider", FeatureState::kEnabled));
    CHECK(qol::infra::capability_count() == 1);
    CHECK(require_attr_range() == &g_provider_api);
    CHECK(g_provider_enabled_calls == 1);
}

void test_installed_disabled_does_not_register() {
    reset_all();
    // gate 未设置默认 false → installed_disabled，不注册。
    CHECK(register_plugin(make_provider()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("cap_provider", FeatureState::kInstalledDisabled));
    CHECK(qol::infra::capability_count() == 0);
    CHECK(require_attr_range() == nullptr);
    CHECK(g_provider_disabled_calls == 1);
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

    const AttrRangeApi* saved = require_attr_range();
    CHECK(saved != nullptr);
    g_unhook_calls = 0;

    // 关闭 provider 门禁：下一轮 sync_gates 走撤销路径（先 revoking，再级联，再注销）。
    qol::infra::config_set_bool("cap.provider.on", false);
    CHECK(sync_gates() >= 1);

    CHECK(qol::infra::capability_count() == 0);
    CHECK(require_attr_range() == nullptr);
    CHECK(g_provider_disabled_calls == 1);  // 提供者门禁已关
    CHECK(g_provider_revoking);
    // 撤销期新调用被提供者拒绝（能力仍在撤销事务内，指针尚未失效）。
    CHECK(!saved->probe_jewel_range(0, nullptr, nullptr, nullptr));

    // required 消费者：installed_disabled + reason=cascade_stopped + 资源已释放。
    FeatureStatus req{};
    CHECK(feature_status("cap_consumer_req", &req));
    CHECK(req.state == FeatureState::kInstalledDisabled);
    CHECK(!req.enabled);
    CHECK(req.error != nullptr && std::strcmp(req.error, "cascade_stopped") == 0);
    CHECK(g_req_disabled_calls == 1);
    CHECK(qol::hooks::state_of("hook:cap:req") == ResourceState::kFree);
    CHECK(g_unhook_calls == 1);

    // optional 消费者：仅 degraded 标记，生命周期态与资源不变。
    FeatureStatus opt_after{};
    CHECK(feature_status("cap_consumer_opt", &opt_after));
    CHECK(opt_after.degraded);
    CHECK(opt_after.state == FeatureState::kEnabled);
    CHECK(qol::hooks::state_of("hook:cap:opt") == ResourceState::kLive);
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
    CHECK(require_attr_range() == nullptr);
    CHECK(g_provider_disabled_calls == 1);   // 撤销期关提供者门禁
    CHECK(g_provider_deactivate_calls == 1); // 终态插件自身清理
    CHECK(qol::hooks::state_of("hook:cap:prov") == ResourceState::kFree);
}

}  // namespace

int main() {
    test_enabled_registers_capability();
    test_installed_disabled_does_not_register();
    test_disable_revokes_and_cascades();
    test_no_auto_recovery();
    test_deactivate_revokes_capability();
    std::printf("capability_lifecycle: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
