// host 生命周期全路径 host 单测（P3 Lane B）：
// 用假描述符 + 假 tx 依赖（resolve/hook/unhook/gate）+ 真实 registry/tx/status/config，
// 覆盖 activate/deactivate 事务全路径、enable/disable 幂等与非法迁移、busy、isolated、
// 失败事务（unhook 失败）→ kFailed + tombstone/isolated。

#include "host/plugin_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

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
using qol::infra::FeatureState;

namespace {

// ---------------------------------------------------------------------------
// fake tx 后端
// ---------------------------------------------------------------------------

struct FakeBackend {
    int hook_calls = 0;
    int unhook_calls = 0;
    int hook_rc = 0;
    int unhook_rc = 0;
    uintptr_t resolve_rc = 0x1000;
};

FakeBackend g_backend;

uintptr_t fake_resolve(const char* symbol, bool* out_followed) {
    (void)symbol;
    if (out_followed != nullptr) *out_followed = false;
    return g_backend.resolve_rc;
}

int fake_hook(void* target, void* replacement, void** backup) {
    (void)target;
    (void)replacement;
    ++g_backend.hook_calls;
    if (g_backend.hook_rc != 0) return g_backend.hook_rc;
    if (backup != nullptr) {
        *backup = reinterpret_cast<void*>(static_cast<uintptr_t>(0xA000 + g_backend.hook_calls));
    }
    return 0;
}

int fake_unhook(void* target) {
    (void)target;
    ++g_backend.unhook_calls;
    return g_backend.unhook_rc;
}

bool fake_gate(const char* key) {
    return key == nullptr ? true : qol::infra::config_get_bool(key, false);
}

void set_deps() {
    qol::hooks::tx_set_deps(qol::hooks::TxDeps{&fake_resolve, &fake_hook, &fake_unhook, &fake_gate});
}

HostDeps make_host_deps() {
    return HostDeps{&fake_resolve, &fake_hook, &fake_unhook};
}

// ---------------------------------------------------------------------------
// 探针插件
// ---------------------------------------------------------------------------

int g_dummy_handler = 0;

struct PluginProbe {
    int install_calls = 0;
    int deactivate_calls = 0;
    int enabled_calls = 0;
    int disabled_calls = 0;
    int ready_calls = 0;
    void* ready_backups[4] = {};
};

PluginProbe g_probe;

int probe_install(PluginContext&) {
    ++g_probe.install_calls;
    return 0;
}
void probe_deactivate(PluginContext&) { ++g_probe.deactivate_calls; }
void probe_enabled(PluginContext&) { ++g_probe.enabled_calls; }
void probe_disabled(PluginContext&) { ++g_probe.disabled_calls; }
void probe_ready(PluginContext&, const char* resource_id, void* backup) {
    (void)resource_id;
    ++g_probe.ready_calls;
    if (g_probe.ready_calls <= 4) {
        g_probe.ready_backups[g_probe.ready_calls - 1] = backup;
    }
}
void probe_fill_status(PluginStatusOut& out) {
    out.enabled = false;
    out.error = nullptr;
}

ResourceClaim claim(const char* id, const char* owner, const char* gate) {
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

ResourceClaim g_resources[] = {
    claim("hook:lc:a", "test_lc", "test.gate"),
    claim("hook:lc:b", "test_lc", "test.gate"),
};

PluginDescriptor make_descriptor() {
    PluginDescriptor d{};
    d.id = "test_lc";
    d.version = "0.1.0";
    d.requires_count = 0;
    d.requires = nullptr;
    d.optional_count = 0;
    d.optional_requires = nullptr;
    d.provides_count = 0;
    d.provides = nullptr;
    d.resources_count = 2;
    d.resources = g_resources;
    d.jni_count = 0;
    d.jni = nullptr;
    d.install = &probe_install;
    d.deactivate = &probe_deactivate;
    d.on_enabled = &probe_enabled;
    d.on_disabled = &probe_disabled;
    d.fill_status = &probe_fill_status;
    d.on_resource_ready = &probe_ready;
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
    g_backend = FakeBackend{};
    g_probe = PluginProbe{};
    set_deps();
}

bool feature_state_is(const char* id, FeatureState expected) {
    qol::infra::FeatureStatus status{};
    return qol::infra::status_get(id, &status) && status.state == expected;
}

// 反复推进生命周期事务直到无事可做。
int drive_lifecycle() {
    int steps = 0;
    while (lifecycle_step() > 0 && steps < 1000) {
        ++steps;
    }
    return steps;
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

void test_activate_gate_off() {
    reset_all();
    CHECK(register_plugin(make_descriptor()));
    CHECK(qol::infra::status_register("test_lc", "0.1.0"));
    CHECK(qol::infra::status_update("test_lc", FeatureState::kInactive, false, nullptr));

    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    CHECK(feature_state_is("test_lc", FeatureState::kResolving));
    // 事务在途：第二个动作被 busy 拒绝。
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));

    drive_lifecycle();
    // gate 关闭（配置默认 false）→ installed_disabled + 资源 disabled。
    CHECK(feature_state_is("test_lc", FeatureState::kInstalledDisabled));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kDisabled);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kDisabled);
    // backup 经 on_resource_ready 交还（两条资源各一次）。
    CHECK(g_probe.ready_calls == 2);
    CHECK(g_probe.ready_backups[0] != nullptr);
    CHECK(g_probe.ready_backups[1] != nullptr);
    CHECK(g_probe.disabled_calls == 1);
    CHECK(g_probe.enabled_calls == 0);
}

void test_activate_gate_on() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    CHECK(register_plugin(make_descriptor()));
    CHECK(qol::infra::status_register("test_lc", "0.1.0"));
    CHECK(qol::infra::status_update("test_lc", FeatureState::kInactive, false, nullptr));

    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    drive_lifecycle();
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kLive);
    CHECK(g_probe.ready_calls == 2);
    CHECK(g_probe.enabled_calls == 1);
}

void test_deactivate_returns_to_inactive() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    CHECK(register_plugin(make_descriptor()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));

    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDeactivate));
    CHECK(feature_state_is("test_lc", FeatureState::kDeactivating));
    drive_lifecycle();
    CHECK(feature_state_is("test_lc", FeatureState::kInactive));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kFree);
    CHECK(g_backend.unhook_calls == 2);
}

void test_enable_disable_idempotent_and_illegal() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    CHECK(register_plugin(make_descriptor()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));

    // enable 幂等。
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleEnable));
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));

    // disable → installed_disabled；再次 disable 幂等。
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDisable));
    CHECK(feature_state_is("test_lc", FeatureState::kInstalledDisabled));
    CHECK(g_probe.disabled_calls == 1);
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDisable));
    CHECK(g_probe.disabled_calls == 1);

    // 非法迁移：installed_disabled 不可 activate（仅 inactive 可入）。
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));

    // enable 回到 enabled。
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleEnable));
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));
    CHECK(g_probe.enabled_calls == 2);  // install 一次 + enable 一次

    // 未知插件 / 未知动作。
    CHECK(!lifecycle_apply("no_such_plugin", qol::contracts::kFeatureLifecycleEnable));
    CHECK(!lifecycle_apply("test_lc", 99));
    CHECK(!lifecycle_apply(nullptr, qol::contracts::kFeatureLifecycleEnable));
}

void test_isolated_rejects_lifecycle() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    CHECK(register_plugin(make_descriptor()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));

    CHECK(qol::hooks::isolated_set("hook:lc:a", true));
    // 任一资源隔离 → 前置拒绝（不改变状态、不启动事务）。
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDeactivate));
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kLive);
}

void test_failed_release_tombstones_and_isolates() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    CHECK(register_plugin(make_descriptor()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));

    g_backend.unhook_rc = -1;  // 物理归还原语失败
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDeactivate));
    drive_lifecycle();

    CHECK(feature_state_is("test_lc", FeatureState::kFailed));
    // 逆序还原：先处理 b，unhook 失败 → tombstone + isolated；a 未处理仍 live。
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kTombstone);
    CHECK(qol::hooks::isolated_get("hook:lc:b"));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kLive);

    // 隔离后同插件生命周期动作继续被拒。
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDeactivate));
}

}  // namespace

int main() {
    test_activate_gate_off();
    test_activate_gate_on();
    test_deactivate_returns_to_inactive();
    test_enable_disable_idempotent_and_illegal();
    test_isolated_rejects_lifecycle();
    test_failed_release_tombstones_and_isolates();
    std::printf("plugin_lifecycle: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
