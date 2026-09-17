// host/plugin_registry.h host 单测（fake 后端，脱离游戏进程）：
// 覆盖成功安装、门禁关闭、部分失败回滚、回滚失败 tombstone、重复 id、
// 缺 requires 阻断与注册表 reset。资源安装原语与符号解析均由 HostDeps 注入 fake。

#include "host/plugin_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/plugin.h"
#include "contracts/ownership.h"
#include "hooks/registry.h"
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
using qol::host::find_plugin;
using qol::host::HostDeps;
using qol::host::install_all;
using qol::host::plugin_count;
using qol::host::plugin_registry_reset_for_test;
using qol::host::register_plugin;
using qol::host::sync_gates;
using qol::infra::FeatureState;

namespace {

// ---------------------------------------------------------------------------
// fake 后端
// ---------------------------------------------------------------------------

struct FakeBackend {
    int hook_calls = 0;
    int unhook_calls = 0;
    int hook_rc = 0;
    int unhook_rc = 0;
    int fail_hook_call = 0;  // 第 N 次 hook 调用（1 起）返回失败；0 = 不失败
    uintptr_t resolve_rc = 0x1000;
    void* last_target = nullptr;
    void* last_replacement = nullptr;
};

FakeBackend g_backend;

uintptr_t fake_resolve(const char* symbol, bool* out_followed) {
    (void)symbol;
    if (out_followed != nullptr) *out_followed = false;
    return g_backend.resolve_rc;
}

int fake_hook(void* target, void* replacement, void** backup) {
    ++g_backend.hook_calls;
    g_backend.last_target = target;
    g_backend.last_replacement = replacement;
    if (g_backend.fail_hook_call != 0 && g_backend.hook_calls == g_backend.fail_hook_call) {
        return -1;
    }
    if (g_backend.hook_rc != 0) return g_backend.hook_rc;
    if (backup != nullptr) {
        *backup = reinterpret_cast<void*>(static_cast<uintptr_t>(0x9000 + g_backend.hook_calls));
    }
    return 0;
}

int fake_unhook(void* target) {
    (void)target;
    ++g_backend.unhook_calls;
    return g_backend.unhook_rc;
}

HostDeps make_deps() {
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
    int install_rc = 0;
    char last_ready_id[64] = {};
    void* ready_backups[4] = {};
};

PluginProbe g_probe;

int probe_install(PluginContext&) {
    ++g_probe.install_calls;
    return g_probe.install_rc;
}
void probe_deactivate(PluginContext&) { ++g_probe.deactivate_calls; }
void probe_enabled(PluginContext&) { ++g_probe.enabled_calls; }
void probe_disabled(PluginContext&) { ++g_probe.disabled_calls; }
int32_t probe_ready(PluginContext&, const char* resource_id, void* backup) {
    ++g_probe.ready_calls;
    if (resource_id != nullptr) {
        std::snprintf(g_probe.last_ready_id, sizeof(g_probe.last_ready_id), "%s", resource_id);
    }
    if (g_probe.ready_calls <= 4) {
        g_probe.ready_backups[g_probe.ready_calls - 1] = backup;
    }
    return 0;
}
void probe_fill_status(PluginStatusOut& out) {
    out.enabled = false;
    out.error = nullptr;
}

// kOwnership 探针：记录 acquire/release 调用与顺序（最多 8 条）。
struct OwnershipProbe {
    int acquire_calls = 0;
    int release_calls = 0;
    int fail_acquire_at = 0;  // 1-based；0 = 不失败
    char acquire_order[8][32] = {};
    char release_order[8][32] = {};
};

OwnershipProbe g_ownership;

int32_t ownership_acquire_probe(const char* id) {
    const int call = ++g_ownership.acquire_calls;
    if (id != nullptr && call <= 8) {
        std::snprintf(g_ownership.acquire_order[call - 1], sizeof(g_ownership.acquire_order[0]),
                      "%s", id);
    }
    if (g_ownership.fail_acquire_at != 0 && call == g_ownership.fail_acquire_at) return -1;
    return 0;
}

void ownership_release_probe(const char* id) {
    const int call = ++g_ownership.release_calls;
    if (id != nullptr && call <= 8) {
        std::snprintf(g_ownership.release_order[call - 1], sizeof(g_ownership.release_order[0]),
                      "%s", id);
    }
}

const qol::contracts::OwnershipOps g_ownership_ops = {&ownership_acquire_probe,
                                                      &ownership_release_probe};

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

// optional + fail-soft 入口 hook 资源（install 失败只置 degraded、不整体失败，§8）。
ResourceClaim optional_claim(const char* id, const char* owner) {
    ResourceClaim c = claim(id, owner, nullptr);
    c.requirement = ResourceRequirement::kOptional;
    c.failure = ResourceFailure::kFailSoft;
    return c;
}

// kOwnership 资源：handler 指向 OwnershipOps；symbol 仅占位（机制不经站点解析）。
ResourceClaim ownership_claim(const char* id, const char* owner) {
    ResourceClaim c{};
    c.id = id;
    c.symbol = "ownership:test";
    c.mechanism = ResourceMechanism::kOwnership;
    c.policy = ResourcePolicy::kPrivate;
    c.requirement = ResourceRequirement::kRequired;
    c.failure = ResourceFailure::kFailClosed;
    c.resumable = true;
    c.owner = owner;
    c.reason = "test";
    c.gate = nullptr;
    c.step_budget = 120;
    c.handler = const_cast<qol::contracts::OwnershipOps*>(&g_ownership_ops);
    return c;
}

// required × fail-soft + gate：安装失败应保持 free/degraded、可重试（§8，门禁路径）。
ResourceClaim ownership_soft_claim(const char* id, const char* owner, const char* gate) {
    ResourceClaim c = ownership_claim(id, owner);
    c.failure = ResourceFailure::kFailSoft;
    c.gate = gate;
    return c;
}

ResourceClaim g_ok_resources[] = {
    claim("hook:test:ok:a", "test_ok", nullptr),
    claim("hook:test:ok:b", "test_ok", nullptr),
};
ResourceClaim g_gate_resources[] = {
    claim("hook:test:gate:a", "test_gate", "test.gate"),
};
ResourceClaim g_fail_resources[] = {
    claim("hook:test:fail:a", "test_fail", nullptr),
    claim("hook:test:fail:b", "test_fail", nullptr),
};
ResourceClaim g_gate2_resources[] = {
    claim("hook:test:gate2:a", "test_gate2", "test.gate2"),
    claim("hook:test:gate2:b", "test_gate2", "test.gate2"),
};

CapabilityRequirement g_missing_requires[] = {
    {"cap:missing", QolVersionRange{1, 0}},
};

// optional/fail-soft：第 2 条为 optional+fail-soft，注入 hook 失败时其余仍装、插件 degraded。
ResourceClaim g_optional_resources[] = {
    claim("hook:test:opt:a", "test_opt", nullptr),
    optional_claim("hook:test:opt:b", "test_opt"),
    claim("hook:test:opt:c", "test_opt", nullptr),
};

// required+fail-closed 失败必须整体回滚（即使存在 optional 资源）。
ResourceClaim g_required_then_optional[] = {
    claim("hook:test:req:a", "test_req", nullptr),
    claim("hook:test:req:b", "test_req", nullptr),
    optional_claim("hook:test:req:c", "test_req"),
};

// kOwnership 分派：三条按声明序 acquire；失败时逆序 release。
ResourceClaim g_ownership_resources[] = {
    ownership_claim("own:test:a", "test_own"),
    ownership_claim("own:test:b", "test_own"),
    ownership_claim("own:test:c", "test_own"),
};

// 门禁 fail-soft：acquire 失败不得 tombstone/failed，保持 free 供逐帧重试。
ResourceClaim g_own_soft_resources[] = {
    ownership_soft_claim("own:soft:a", "test_own_soft", "test.gate"),
};

PluginDescriptor make_descriptor(const char* id, const ResourceClaim* resources,
                                 uint32_t resource_count, const CapabilityRequirement* requires,
                                 uint32_t requires_count) {
    PluginDescriptor d{};
    d.id = id;
    d.version = "0.1.0";
    d.requires_count = requires_count;
    d.requires = requires;
    d.optional_count = 0;
    d.optional_requires = nullptr;
    d.provides_count = 0;
    d.provides = nullptr;
    d.resources_count = resource_count;
    d.resources = resources;
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
    qol::infra::status_reset_for_test();
    qol::infra::capabilities_reset_for_test();
    qol::infra::config_reset_for_test();
    qol::infra::events_reset_for_test();
    g_backend = FakeBackend{};
    g_probe = PluginProbe{};
    g_ownership = OwnershipProbe{};
}

bool feature_of(const char* id, FeatureState* state, bool* enabled, const char** error) {
    qol::infra::FeatureStatus features[8];
    uint32_t written = 0;
    if (!qol::infra::status_snapshot(features, 8, &written)) return false;
    for (uint32_t i = 0; i < written; ++i) {
        if (features[i].id != nullptr && std::strcmp(features[i].id, id) == 0) {
            if (state != nullptr) *state = features[i].state;
            if (enabled != nullptr) *enabled = features[i].enabled;
            if (error != nullptr) *error = features[i].error;
            return true;
        }
    }
    return false;
}

bool feature_state_is(const char* id, FeatureState expected) {
    FeatureState state = FeatureState::kRegistered;
    return feature_of(id, &state, nullptr, nullptr) && state == expected;
}

bool feature_degraded(const char* id) {
    qol::infra::FeatureStatus features[8];
    uint32_t written = 0;
    if (!qol::infra::status_snapshot(features, 8, &written)) return false;
    for (uint32_t i = 0; i < written; ++i) {
        if (features[i].id != nullptr && std::strcmp(features[i].id, id) == 0) {
            return features[i].degraded;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

void test_success_path() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_ok", g_ok_resources, 2, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(plugin_count() == 1);
    CHECK(find_plugin("test_ok") != nullptr);

    CHECK(install_all(make_deps()) == 0);
    CHECK(g_backend.hook_calls == 2);
    CHECK(g_probe.install_calls == 1);
    CHECK(g_probe.enabled_calls == 1);
    CHECK(g_probe.disabled_calls == 0);
    CHECK(g_probe.ready_calls == 2);
    CHECK(g_probe.ready_backups[0] == reinterpret_cast<void*>(0x9001));
    CHECK(g_probe.ready_backups[1] == reinterpret_cast<void*>(0x9002));
    CHECK(qol::hooks::state_of("hook:test:ok:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:test:ok:b") == ResourceState::kLive);

    FeatureState state = FeatureState::kRegistered;
    bool enabled = false;
    const char* error = nullptr;
    CHECK(feature_of("test_ok", &state, &enabled, &error));
    CHECK(state == FeatureState::kEnabled);
    CHECK(enabled);
    CHECK(error == nullptr);
}

void test_gate_off_installed_disabled() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_gate", g_gate_resources, 1, nullptr, 0);
    CHECK(register_plugin(d));
    // 配置键未设置时默认 false。
    CHECK(install_all(make_deps()) == 0);
    CHECK(qol::hooks::state_of("hook:test:gate:a") == ResourceState::kDisabled);
    CHECK(g_probe.enabled_calls == 0);
    CHECK(g_probe.disabled_calls == 1);

    FeatureState state = FeatureState::kRegistered;
    bool enabled = true;
    CHECK(feature_of("test_gate", &state, &enabled, nullptr));
    CHECK(state == FeatureState::kInstalledDisabled);
    CHECK(!enabled);
}

void test_gate_on_and_config_hot_switch() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    const PluginDescriptor d =
        make_descriptor("test_gate", g_gate_resources, 1, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(qol::hooks::state_of("hook:test:gate:a") == ResourceState::kLive);
    CHECK(g_probe.enabled_calls == 1);
    CHECK(g_probe.disabled_calls == 0);
}

void test_second_resource_failure_rolls_back() {
    reset_all();
    g_backend.fail_hook_call = 2;
    g_backend.unhook_rc = 0;
    const PluginDescriptor d =
        make_descriptor("test_fail", g_fail_resources, 2, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 1);
    CHECK(g_backend.hook_calls == 2);
    CHECK(g_backend.unhook_calls == 1);
    CHECK(qol::hooks::state_of("hook:test:fail:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:fail:b") == ResourceState::kFree);
    CHECK(g_probe.ready_calls == 1);   // 仅第一条资源交还过 backup
    CHECK(g_probe.enabled_calls == 0);
    CHECK(g_probe.disabled_calls == 0);

    FeatureState state = FeatureState::kRegistered;
    bool enabled = true;
    const char* error = nullptr;
    CHECK(feature_of("test_fail", &state, &enabled, &error));
    CHECK(state == FeatureState::kFailed);
    CHECK(!enabled);
    CHECK(error != nullptr);
    CHECK(std::strstr(error, "hook:test:fail:b") != nullptr);
}

void test_unhook_failure_leaves_tombstone() {
    reset_all();
    g_backend.fail_hook_call = 2;
    g_backend.unhook_rc = -1;
    const PluginDescriptor d =
        make_descriptor("test_fail", g_fail_resources, 2, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 1);
    CHECK(g_backend.unhook_calls == 1);
    CHECK(qol::hooks::state_of("hook:test:fail:a") == ResourceState::kTombstone);
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 1);

    FeatureState state = FeatureState::kRegistered;
    CHECK(feature_of("test_fail", &state, nullptr, nullptr));
    CHECK(state == FeatureState::kFailed);
}

void test_missing_requires_blocks() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_requires", nullptr, 0, g_missing_requires, 1);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 1);
    CHECK(g_probe.install_calls == 0);
    FeatureState state = FeatureState::kRegistered;
    bool enabled = true;
    const char* error = nullptr;
    CHECK(feature_of("test_requires", &state, &enabled, &error));
    CHECK(state == FeatureState::kBlocked);
    CHECK(!enabled);
    CHECK(error != nullptr);
}

void test_install_failure_marks_failed() {
    reset_all();
    g_probe.install_rc = -1;
    const PluginDescriptor d =
        make_descriptor("test_ok", g_ok_resources, 2, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 1);
    CHECK(g_backend.hook_calls == 0);
    FeatureState state = FeatureState::kRegistered;
    CHECK(feature_of("test_ok", &state, nullptr, nullptr));
    CHECK(state == FeatureState::kFailed);
}

void test_duplicate_and_invalid_registration() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_ok", g_ok_resources, 2, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(!register_plugin(d));   // 重复 id
    CHECK(plugin_count() == 1);

    PluginDescriptor invalid = d;
    invalid.id = nullptr;
    CHECK(!register_plugin(invalid));

    PluginDescriptor empty_id = d;
    empty_id.id = "";
    CHECK(!register_plugin(empty_id));

    PluginDescriptor null_install = make_descriptor("test_no_install", nullptr, 0, nullptr, 0);
    null_install.install = nullptr;
    CHECK(!register_plugin(null_install));

    CHECK(plugin_count() == 1);
}

void test_sync_gates_hot_switch() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_gate", g_gate_resources, 1, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("test_gate", FeatureState::kInstalledDisabled));
    CHECK(qol::hooks::state_of("hook:test:gate:a") == ResourceState::kDisabled);
    CHECK(g_probe.enabled_calls == 0);
    CHECK(g_probe.disabled_calls == 1);

    // 配置开启：下一轮同步应把资源置 live、插件置 enabled，并调 on_enabled。
    qol::infra::config_set_bool("test.gate", true);
    CHECK(sync_gates() == 1);
    CHECK(feature_state_is("test_gate", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:test:gate:a") == ResourceState::kLive);
    CHECK(g_probe.enabled_calls == 1);
    CHECK(g_probe.disabled_calls == 1);

    // 幂等：无变化返回 0。
    CHECK(sync_gates() == 0);
    CHECK(g_probe.enabled_calls == 1);

    // 配置关闭：回 installed_disabled/disabled，并调 on_disabled。
    qol::infra::config_set_bool("test.gate", false);
    CHECK(sync_gates() == 1);
    CHECK(feature_state_is("test_gate", FeatureState::kInstalledDisabled));
    CHECK(qol::hooks::state_of("hook:test:gate:a") == ResourceState::kDisabled);
    CHECK(g_probe.enabled_calls == 1);
    CHECK(g_probe.disabled_calls == 2);
    CHECK(sync_gates() == 0);
}

void test_sync_gates_leaves_tombstone() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_gate2", g_gate2_resources, 2, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(qol::hooks::state_of("hook:test:gate2:a") == ResourceState::kDisabled);
    CHECK(qol::hooks::state_of("hook:test:gate2:b") == ResourceState::kDisabled);

    // 人为置 tombstone：门禁翻转不得改写保留占用，另一条正常同步。
    CHECK(qol::hooks::set_state("hook:test:gate2:a", ResourceState::kTombstone));
    qol::infra::config_set_bool("test.gate2", true);
    CHECK(sync_gates() == 1);
    CHECK(feature_state_is("test_gate2", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:test:gate2:a") == ResourceState::kTombstone);
    CHECK(qol::hooks::state_of("hook:test:gate2:b") == ResourceState::kLive);
}

void test_reset_is_clean() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_ok", g_ok_resources, 2, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(plugin_count() == 1);
    plugin_registry_reset_for_test();
    CHECK(plugin_count() == 0);
    CHECK(find_plugin("test_ok") == nullptr);
    CHECK(find_plugin(nullptr) == nullptr);
}

// 零资源插件（如 quest）：无资源声明、无门禁，install_all 后直接 enabled；
// 不触 hook/unhook/on_resource_ready，fill_status 可调用。
void test_zero_resource_plugin() {
    reset_all();
    const PluginDescriptor d = make_descriptor("test_zero", nullptr, 0, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(plugin_count() == 1);

    CHECK(install_all(make_deps()) == 0);
    CHECK(g_backend.hook_calls == 0);
    CHECK(g_backend.unhook_calls == 0);
    CHECK(g_probe.install_calls == 1);
    CHECK(g_probe.enabled_calls == 1);
    CHECK(g_probe.disabled_calls == 0);
    CHECK(g_probe.ready_calls == 0);

    FeatureState state = FeatureState::kRegistered;
    bool enabled = false;
    const char* error = nullptr;
    CHECK(feature_of("test_zero", &state, &enabled, &error));
    CHECK(state == FeatureState::kEnabled);
    CHECK(enabled);
    CHECK(error == nullptr);

    // fill_status 正常可调用（probe 固定写 enabled=false / error=nullptr）。
    PluginStatusOut out{};
    d.fill_status(out);
    CHECK(!out.enabled);
    CHECK(out.error == nullptr);

    // 无门禁资源：sync_gates 不参与监督，幂等返回 0。
    CHECK(sync_gates() == 0);
    CHECK(g_probe.enabled_calls == 1);
    CHECK(g_probe.disabled_calls == 0);
}

// §8 optional × fail-soft：第 2 条 optional 入口 hook 安装失败 → 只置 degraded，
// 跳过该资源继续安装其余；插件仍 enabled，失败资源保持 free。
void test_optional_fail_soft_degraded() {
    reset_all();
    g_backend.fail_hook_call = 2;
    const PluginDescriptor d =
        make_descriptor("test_opt", g_optional_resources, 3, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 0);  // 不算失败插件
    CHECK(g_backend.hook_calls == 3);      // 失败后继续装第 3 条
    CHECK(g_backend.unhook_calls == 0);    // 不回滚已装成功的资源
    CHECK(qol::hooks::state_of("hook:test:opt:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:test:opt:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:opt:c") == ResourceState::kLive);

    FeatureState state = FeatureState::kRegistered;
    bool enabled = false;
    const char* error = nullptr;
    CHECK(feature_of("test_opt", &state, &enabled, &error));
    CHECK(state == FeatureState::kEnabled);
    CHECK(enabled);
    CHECK(feature_degraded("test_opt"));
    CHECK(error != nullptr);
    CHECK(std::strstr(error, "hook:test:opt:b") != nullptr);
    CHECK(g_probe.enabled_calls == 1);
    CHECK(g_probe.disabled_calls == 0);
}

// §8 required × fail-closed：失败必须逆序回滚已装资源并置 failed；
// 即使其后还有 optional 资源也不得继续安装。
void test_required_fail_closed_rolls_back_with_optional() {
    reset_all();
    g_backend.fail_hook_call = 2;
    const PluginDescriptor d =
        make_descriptor("test_req", g_required_then_optional, 3, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 1);
    CHECK(g_backend.hook_calls == 2);    // 第 2 条失败后中止，不再装第 3 条
    CHECK(g_backend.unhook_calls == 1);  // 逆序回滚第 1 条
    CHECK(qol::hooks::state_of("hook:test:req:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:req:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:req:c") == ResourceState::kFree);
    CHECK(feature_state_is("test_req", FeatureState::kFailed));
    CHECK(!feature_degraded("test_req"));
}

// kOwnership 分派：claim.handler 指向 OwnershipOps，acquire 按声明序调用；
// 全部成功 → 资源 live，不经 patch 站点、不触 hook/unhook。
void test_ownership_acquire_order_live() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_own", g_ownership_resources, 3, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 0);
    CHECK(g_ownership.acquire_calls == 3);
    CHECK(g_ownership.release_calls == 0);
    CHECK(std::strcmp(g_ownership.acquire_order[0], "own:test:a") == 0);
    CHECK(std::strcmp(g_ownership.acquire_order[1], "own:test:b") == 0);
    CHECK(std::strcmp(g_ownership.acquire_order[2], "own:test:c") == 0);
    CHECK(g_backend.hook_calls == 0);
    CHECK(g_backend.unhook_calls == 0);
    CHECK(qol::hooks::state_of("own:test:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("own:test:b") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("own:test:c") == ResourceState::kLive);
    CHECK(feature_state_is("test_own", FeatureState::kEnabled));
}

// kOwnership 回滚：第 3 条 acquire 失败 → 逆序 release 第 2、第 1 条；插件 failed。
void test_ownership_release_reverse_on_rollback() {
    reset_all();
    g_ownership.fail_acquire_at = 3;
    const PluginDescriptor d =
        make_descriptor("test_own", g_ownership_resources, 3, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 1);
    CHECK(g_ownership.acquire_calls == 3);
    CHECK(g_ownership.release_calls == 2);
    CHECK(std::strcmp(g_ownership.release_order[0], "own:test:b") == 0);  // 逆序
    CHECK(std::strcmp(g_ownership.release_order[1], "own:test:a") == 0);
    CHECK(qol::hooks::state_of("own:test:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("own:test:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("own:test:c") == ResourceState::kFree);
    CHECK(feature_state_is("test_own", FeatureState::kFailed));
}

// 门禁 fail-soft（§8）：kOwnership required×fail-soft acquire 失败 → 资源保持 free（可重试）、
// 插件 enabled + degraded、error 记录原因、不置 tombstone；下一逻辑帧重试成功 → live + 清 degraded。
void test_ownership_fail_soft_gate_retry() {
    reset_all();
    qol::infra::config_set_bool("test.gate", true);
    g_ownership.fail_acquire_at = 1;
    const PluginDescriptor d =
        make_descriptor("test_own_soft", g_own_soft_resources, 1, nullptr, 0);
    CHECK(register_plugin(d));

    // 启动安装：acquire 失败 → fail-soft（不得 tombstone / 插件 failed）。
    CHECK(install_all(make_deps()) == 0);
    CHECK(g_ownership.acquire_calls == 1);
    CHECK(qol::hooks::state_of("own:soft:a") == ResourceState::kFree);
    CHECK(!qol::hooks::isolated_get("own:soft:a"));
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
    CHECK(feature_state_is("test_own_soft", FeatureState::kEnabled));
    CHECK(feature_degraded("test_own_soft"));
    {
        FeatureState state = FeatureState::kRegistered;
        bool enabled = false;
        const char* error = nullptr;
        CHECK(feature_of("test_own_soft", &state, &enabled, &error));
        CHECK(enabled);
        CHECK(error != nullptr);
        CHECK(std::strstr(error, "own:soft:a") != nullptr);
    }

    // 门禁每逻辑帧重试：本轮 acquire 成功 → live + 清 degraded。
    g_ownership.fail_acquire_at = 0;
    sync_gates();
    CHECK(g_ownership.acquire_calls == 2);
    CHECK(qol::hooks::state_of("own:soft:a") == ResourceState::kLive);
    CHECK(!feature_degraded("test_own_soft"));
    CHECK(feature_state_is("test_own_soft", FeatureState::kEnabled));
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
}

}  // namespace

int main() {
    test_success_path();
    test_gate_off_installed_disabled();
    test_gate_on_and_config_hot_switch();
    test_second_resource_failure_rolls_back();
    test_unhook_failure_leaves_tombstone();
    test_missing_requires_blocks();
    test_install_failure_marks_failed();
    test_duplicate_and_invalid_registration();
    test_sync_gates_hot_switch();
    test_sync_gates_leaves_tombstone();
    test_reset_is_clean();
    test_zero_resource_plugin();
    test_optional_fail_soft_degraded();
    test_required_fail_closed_rolls_back_with_optional();
    test_ownership_acquire_order_live();
    test_ownership_release_reverse_on_rollback();
    test_ownership_fail_soft_gate_retry();
    std::printf("plugin_registry: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
