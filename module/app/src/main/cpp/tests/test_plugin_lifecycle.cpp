// host 生命周期全路径 host 单测（P3 Lane B）：
// 用假描述符 + 假 tx 依赖（resolve/hook/unhook/gate）+ 真实 registry/tx/status/config，
// 覆盖 activate/deactivate 事务全路径与插件自身回调、安装失败不启动事务、enable/disable
// 被拒（单写者）、busy、isolated、失败事务（unhook 失败）→ kFailed + tombstone/isolated。

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

constexpr const char* kEntrySymbolA = "F_TEST_VMA_LC_A";
constexpr const char* kEntrySymbolB = "F_TEST_VMA_LC_B";
constexpr const char* kEntrySymbolSoft = "F_TEST_VMA_LC_SOFT";

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
    if (out_followed != nullptr) *out_followed = false;
    if (g_backend.resolve_rc == 0 || symbol == nullptr) return 0;
    if (std::strcmp(symbol, kEntrySymbolA) == 0) return g_backend.resolve_rc + 0x100;
    if (std::strcmp(symbol, kEntrySymbolB) == 0) return g_backend.resolve_rc + 0x200;
    if (std::strcmp(symbol, kEntrySymbolSoft) == 0) return g_backend.resolve_rc + 0x300;
    return g_backend.resolve_rc + 0x400;
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
    int install_rc = 0;       // 非 0 = install 回调失败
    int ready_rc = 0;         // 非 0 = on_resource_ready 恒失败
    int ready_fail_call = 0;  // 第 N 次 on_resource_ready（1 起）失败；0 = 不按次数失败
    void* ready_backups[8] = {};
};

PluginProbe g_probe;

int probe_install(PluginContext&) {
    ++g_probe.install_calls;
    return g_probe.install_rc;
}
void probe_deactivate(PluginContext&) { ++g_probe.deactivate_calls; }
void probe_enabled(PluginContext&) { ++g_probe.enabled_calls; }
void probe_disabled(PluginContext&) { ++g_probe.disabled_calls; }
int probe_ready(PluginContext&, const char* resource_id, void* backup) {
    (void)resource_id;
    ++g_probe.ready_calls;
    if (g_probe.ready_calls <= 8) {
        g_probe.ready_backups[g_probe.ready_calls - 1] = backup;
    }
    if (g_probe.ready_fail_call != 0 && g_probe.ready_calls == g_probe.ready_fail_call) {
        return 1;
    }
    return g_probe.ready_rc;
}
void probe_fill_status(PluginStatusOut& out) {
    out.enabled = false;
    out.error = nullptr;
}

ResourceClaim claim(const char* id, const char* owner, const char* gate) {
    ResourceClaim c{};
    c.id = id;
    c.symbol = std::strcmp(id, "hook:lc:a") == 0 ? kEntrySymbolA
               : std::strcmp(id, "hook:lc:b") == 0 ? kEntrySymbolB
                                                    : kEntrySymbolSoft;
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

// D2a-3b-1：带 gate 的 kEntryHook fail-soft 资源（晚就绪重试用例）。
ResourceClaim soft_claim() {
    ResourceClaim c = claim("hook:lc:soft", "test_lc_soft", "test.gate");
    c.failure = ResourceFailure::kFailSoft;
    return c;
}

ResourceClaim g_soft_resources[] = {
    soft_claim(),
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

PluginDescriptor make_soft_descriptor() {
    PluginDescriptor d{};
    d.id = "test_lc_soft";
    d.version = "0.1.0";
    d.requires_count = 0;
    d.requires = nullptr;
    d.optional_count = 0;
    d.optional_requires = nullptr;
    d.provides_count = 0;
    d.provides = nullptr;
    d.resources_count = 1;
    d.resources = g_soft_resources;
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

PluginDescriptor make_zero_descriptor() {
    PluginDescriptor d = make_descriptor();
    d.id = "test_zero_lifecycle";
    d.resources_count = 0;
    d.resources = nullptr;
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
    CHECK(install_all(make_host_deps()) == 0);
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

// 启动装配（同步 install_all）后经 deactivate 事务回到 kInactive。
// 返回 false 表示前置状态未达预期。
bool boot_then_deactivate(bool gate) {
    qol::infra::config_set_bool("test.gate", gate);
    if (!register_plugin(make_descriptor())) return false;
    if (install_all(make_host_deps()) != 0) return false;
    if (!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDeactivate)) return false;
    drive_lifecycle();
    return feature_state_is("test_lc", FeatureState::kInactive);
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

void test_activate_calls_install_and_hands_backup() {
    reset_all();
    // boot 装配：install 回调 1 次、两条资源 backup 交还。
    CHECK(boot_then_deactivate(false));
    CHECK(g_probe.install_calls == 1);
    CHECK(g_probe.deactivate_calls == 1);
    CHECK(g_probe.disabled_calls == 1);  // boot 时 gate 关
    CHECK(g_probe.ready_calls == 2);
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kFree);

    // activate：先调插件 install（第 2 次），再启动安装事务。
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    CHECK(g_probe.install_calls == 2);
    CHECK(feature_state_is("test_lc", FeatureState::kResolving));
    // 事务在途：第二个动作被 busy 拒绝。
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));

    drive_lifecycle();
    // gate 关闭（配置 false）→ installed_disabled + 资源 disabled；backup 再次交还。
    CHECK(feature_state_is("test_lc", FeatureState::kInstalledDisabled));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kDisabled);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kDisabled);
    CHECK(g_probe.ready_calls == 4);
    CHECK(g_probe.ready_backups[2] != nullptr);
    CHECK(g_probe.ready_backups[3] != nullptr);
    CHECK(g_probe.disabled_calls == 2);
    CHECK(g_probe.enabled_calls == 0);
}

void test_activate_gate_on_reentry_is_idempotent() {
    reset_all();
    CHECK(boot_then_deactivate(true));
    CHECK(g_probe.install_calls == 1);
    CHECK(g_probe.deactivate_calls == 1);
    CHECK(g_probe.enabled_calls == 1);  // boot 时 gate 开

    // 第一次 activate。
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    CHECK(g_probe.install_calls == 2);
    drive_lifecycle();
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kLive);
    CHECK(g_probe.enabled_calls == 2);

    // 幂等重入：deactivate → activate 再一轮，install 回调再 +1。
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDeactivate));
    drive_lifecycle();
    CHECK(feature_state_is("test_lc", FeatureState::kInactive));
    CHECK(g_probe.deactivate_calls == 2);
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);

    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    CHECK(g_probe.install_calls == 3);
    drive_lifecycle();
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));
    CHECK(g_probe.enabled_calls == 3);
}

void test_activate_preflights_before_plugin_install() {
    reset_all();
    CHECK(boot_then_deactivate(false));
    CHECK(g_probe.install_calls == 1);
    const int hooks_before = g_backend.hook_calls;
    g_backend.resolve_rc = 0;
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    CHECK(g_probe.install_calls == 1);
    CHECK(g_backend.hook_calls == hooks_before);
    CHECK(feature_state_is("test_lc", FeatureState::kUnsupported));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kFree);
}

void test_zero_resource_lifecycle_is_explicit_noop() {
    reset_all();
    const PluginDescriptor d = make_zero_descriptor();
    CHECK(register_plugin(d));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("test_zero_lifecycle", FeatureState::kEnabled));
    CHECK(g_probe.install_calls == 1);
    CHECK(g_probe.enabled_calls == 1);

    // 无资源插件仍需执行 deactivate 回调并直接进入 inactive，不启动事务。
    CHECK(lifecycle_apply("test_zero_lifecycle", qol::contracts::kFeatureLifecycleDeactivate));
    CHECK(feature_state_is("test_zero_lifecycle", FeatureState::kInactive));
    CHECK(g_probe.deactivate_calls == 1);
    CHECK(lifecycle_step() == 0);

    // activate 仍执行 install，并按无 gate 终态 enabled，不留下 install 副作用。
    CHECK(lifecycle_apply("test_zero_lifecycle", qol::contracts::kFeatureLifecycleActivate));
    CHECK(feature_state_is("test_zero_lifecycle", FeatureState::kEnabled));
    CHECK(g_probe.install_calls == 2);
    CHECK(g_probe.enabled_calls == 2);
    CHECK(lifecycle_step() == 0);
}

void test_install_failure_marks_failed_without_transaction() {
    reset_all();
    CHECK(register_plugin(make_descriptor()));
    CHECK(qol::infra::status_register("test_lc", "0.1.0"));
    CHECK(qol::infra::status_update("test_lc", FeatureState::kInactive, false, nullptr));

    g_probe.install_rc = -1;
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    CHECK(g_probe.install_calls == 1);
    CHECK(feature_state_is("test_lc", FeatureState::kFailed));
    // 未启动事务：无 hook 调用、资源保持 free、无在途可推进。
    CHECK(g_backend.hook_calls == 0);
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kFree);
    CHECK(lifecycle_step() == 0);

    // §3.2：failed 可显式重试——install 恢复后 activate 重新走 resolving → 安装事务；
    // gate 默认关，故终态为 installed_disabled + 资源 disabled。
    g_probe.install_rc = 0;
    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    CHECK(feature_state_is("test_lc", FeatureState::kResolving));
    drive_lifecycle();
    CHECK(g_probe.install_calls == 2);
    CHECK(g_backend.hook_calls == 2);
    CHECK(feature_state_is("test_lc", FeatureState::kInstalledDisabled));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kDisabled);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kDisabled);
}

// 受限回调失败（同步安装路径，第二条资源失败）：全部已装资源按逆序撤回，禁止半装。
void test_resource_ready_failure_rolls_back_installed() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    CHECK(register_plugin(make_descriptor()));
    g_probe.ready_fail_call = 2;  // 第二条资源回调失败
    CHECK(install_all(make_host_deps()) == 1);

    // 两条资源都回到 kFree（含此前已装的第一条），不残留半装。
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kFree);
    CHECK(g_backend.hook_calls == 2);
    CHECK(g_backend.unhook_calls == 2);
    CHECK(g_probe.ready_calls == 2);

    qol::infra::FeatureStatus status{};
    CHECK(qol::infra::status_get("test_lc", &status));
    CHECK(status.state == FeatureState::kFailed);
    CHECK(status.error != nullptr && std::strcmp(status.error, "resource_ready_failed") == 0);
}

// 受限回调失败（activate 事务路径，首条失败）：tx_begin(release) 被在途安装事务拒绝 →
// 同步撤回兜底，后续步新装资源即时撤回；终态不覆盖 failed。
void test_resource_ready_failure_during_activate_fallback() {
    reset_all();
    CHECK(boot_then_deactivate(true));  // boot 期间回调返回 0
    g_probe.ready_fail_call = 3;        // activate 第一步回调失败

    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    drive_lifecycle();

    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kFree);
    qol::infra::FeatureStatus status{};
    CHECK(qol::infra::status_get("test_lc", &status));
    CHECK(status.state == FeatureState::kFailed);
    CHECK(status.error != nullptr && std::strcmp(status.error, "resource_ready_failed") == 0);
}

// 受限回调失败（activate 事务路径，末条失败）：安装事务已终态 → 启动还原事务回滚全部资源。
void test_resource_ready_failure_activate_starts_release_tx() {
    reset_all();
    CHECK(boot_then_deactivate(true));  // boot 期间回调返回 0
    g_probe.ready_fail_call = 4;        // activate 第二步（末条）回调失败

    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleActivate));
    drive_lifecycle();

    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kFree);
    qol::infra::FeatureStatus status{};
    CHECK(qol::infra::status_get("test_lc", &status));
    CHECK(status.state == FeatureState::kFailed);
    CHECK(status.error != nullptr && std::strcmp(status.error, "resource_ready_failed") == 0);
}

void test_deactivate_calls_plugin_and_returns_inactive() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    CHECK(register_plugin(make_descriptor()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));

    CHECK(lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDeactivate));
    CHECK(feature_state_is("test_lc", FeatureState::kDeactivating));
    drive_lifecycle();
    // 终态成功：先调插件自身 deactivate 清理，再置 inactive。
    CHECK(g_probe.deactivate_calls == 1);
    CHECK(feature_state_is("test_lc", FeatureState::kInactive));
    CHECK(qol::hooks::state_of("hook:lc:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:lc:b") == ResourceState::kFree);
    CHECK(g_backend.unhook_calls == 2);
}

void test_enable_disable_rejected() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    CHECK(register_plugin(make_descriptor()));
    CHECK(install_all(make_host_deps()) == 0);
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));
    const int enabled_calls = g_probe.enabled_calls;
    const int disabled_calls = g_probe.disabled_calls;

    // 开关语义归配置门禁：本端点拒绝 enable/disable，状态与回调计数不变。
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleEnable));
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDisable));
    CHECK(feature_state_is("test_lc", FeatureState::kEnabled));
    CHECK(g_probe.enabled_calls == enabled_calls);
    CHECK(g_probe.disabled_calls == disabled_calls);
    // 未知动作同样拒绝。
    CHECK(!lifecycle_apply("test_lc", 99));
    CHECK(!lifecycle_apply("no_such_plugin", qol::contracts::kFeatureLifecycleActivate));
    CHECK(!lifecycle_apply(nullptr, qol::contracts::kFeatureLifecycleActivate));
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
    // 失败终态不调用插件 deactivate。
    CHECK(g_probe.deactivate_calls == 0);

    // 隔离后同插件生命周期动作继续被拒。
    CHECK(!lifecycle_apply("test_lc", qol::contracts::kFeatureLifecycleDeactivate));
}

// D2a-3b-1：带 gate 的 kEntryHook 装配期 fail-soft 失败 → 资源保持 free + 插件 degraded；
// 门禁开且 free 时宿主每逻辑帧重试到成功，backup 交还后清除 degraded（语义等价旧重试线程）。
void test_gated_entry_failsoft_retries_on_sync_gates() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    g_backend.hook_rc = -1;  // 装配期入口 hook 安装失败
    CHECK(register_plugin(make_soft_descriptor()));
    CHECK(install_all(make_host_deps()) == 0);  // fail-soft：不整体失败
    CHECK(qol::hooks::state_of("hook:lc:soft") == ResourceState::kFree);
    CHECK(feature_state_is("test_lc_soft", FeatureState::kEnabled));
    qol::infra::FeatureStatus status{};
    CHECK(qol::infra::status_get("test_lc_soft", &status) && status.degraded);

    // 原语恢复 → 门禁监督重试；安装成功并交还 backup，degraded 清除。
    g_backend.hook_rc = 0;
    qol::host::sync_gates();
    CHECK(qol::hooks::state_of("hook:lc:soft") == ResourceState::kLive);
    CHECK(g_probe.ready_calls == 1);
    CHECK(g_probe.ready_backups[0] != nullptr);
    CHECK(qol::infra::status_get("test_lc_soft", &status));
    CHECK(!status.degraded);
}

}  // namespace

int main() {
    test_activate_calls_install_and_hands_backup();
    test_activate_gate_on_reentry_is_idempotent();
    test_activate_preflights_before_plugin_install();
    test_zero_resource_lifecycle_is_explicit_noop();
    test_install_failure_marks_failed_without_transaction();
    test_resource_ready_failure_rolls_back_installed();
    test_resource_ready_failure_during_activate_fallback();
    test_resource_ready_failure_activate_starts_release_tx();
    test_deactivate_calls_plugin_and_returns_inactive();
    test_enable_disable_rejected();
    test_isolated_rejects_lifecycle();
    test_failed_release_tombstones_and_isolates();
    test_gated_entry_failsoft_retries_on_sync_gates();
    std::printf("plugin_lifecycle: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
