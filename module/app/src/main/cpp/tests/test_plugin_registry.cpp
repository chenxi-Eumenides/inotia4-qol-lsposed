// host/plugin_registry.h host 单测（fake 后端，脱离游戏进程）：
// 覆盖注册约束（重复/非法描述符）、注册表 reset、生产 feature-mode 安装/撤销事务
// 与启动期 ownership NotReady 的 SaveEnter 定向恢复。资源安装原语与符号解析均由 HostDeps 注入 fake。

#include "host/plugin_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "contracts/plugin.h"
#include "contracts/ownership.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"
#include "infra/capabilities.h"
#include "infra/config.h"
#include "infra/events.h"
#include "infra/status.h"
#include "infra/log/qol_log.h"
#include "patch/claims.h"
#include "patch/instr_patch.h"

namespace qol::host {
void runtime_effects_publish_actual(const char*, bool) {}
void runtime_effects_unsubscribe_plugin(const char*) {}
}  // namespace qol::host

// game/access/ptr_slots.cpp 经 save_slot_delete_hook_target 解析槽站点；test_stubs.cpp 提供
// 可控实现，测试经这三个全局量构造站点（host 不依赖真实游戏 GOT）。
namespace qol::game {
extern void* g_ptr_slot_test_addr;
extern uintptr_t g_ptr_slot_test_expected;
extern bool g_ptr_slot_test_resolvable;
}  // namespace qol::game

static int g_pass = 0;
static int g_fail = 0;

namespace {
std::string g_log_capture;
void capture_log(QolLogLevel, QolDomain, const char* line) {
    if (line == nullptr) return;
    g_log_capture.append(line);
    g_log_capture.push_back('\n');
}
}  // namespace

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
using qol::host::plugin_count;
using qol::host::plugin_registry_reset_for_test;
using qol::host::register_plugin;
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
    const char* unresolved_symbol = nullptr;
    const char* alias_symbol_a = nullptr;
    const char* alias_symbol_b = nullptr;
    uintptr_t alias_target = 0;
    const char* last_symbol = nullptr;
    const char* resolved_symbols[128] = {};
    uint32_t resolved_symbol_count = 0;
    uintptr_t follow_target = 0;  // 非 0 = follow 解析器返回的目标地址
    int follow_calls = 0;
    void* last_target = nullptr;
    void* last_replacement = nullptr;
};

FakeBackend g_backend;

uintptr_t fake_resolve(const char* symbol, bool* out_followed) {
    g_backend.last_symbol = symbol;
    if (out_followed != nullptr) *out_followed = false;
    if (symbol != nullptr && g_backend.unresolved_symbol != nullptr &&
        std::strcmp(symbol, g_backend.unresolved_symbol) == 0) return 0;
    if (symbol != nullptr && g_backend.alias_target != 0 &&
        ((g_backend.alias_symbol_a != nullptr &&
          std::strcmp(symbol, g_backend.alias_symbol_a) == 0) ||
         (g_backend.alias_symbol_b != nullptr &&
          std::strcmp(symbol, g_backend.alias_symbol_b) == 0))) {
        return g_backend.alias_target;
    }
    if (g_backend.resolve_rc == 0 || symbol == nullptr) return 0;
    for (uint32_t i = 0; i < g_backend.resolved_symbol_count; ++i) {
        if (std::strcmp(symbol, g_backend.resolved_symbols[i]) == 0) {
            return g_backend.resolve_rc + static_cast<uintptr_t>(i) * 0x100;
        }
    }
    if (g_backend.resolved_symbol_count >= 128) return 0;
    const uint32_t slot = g_backend.resolved_symbol_count++;
    g_backend.resolved_symbols[slot] = symbol;
    return g_backend.resolve_rc + static_cast<uintptr_t>(slot) * 0x100;
}

// fake follow 解析：follow_target 非 0 时返回它；否则回退普通解析。
uintptr_t fake_resolve_follow(const char* symbol, bool* out_followed) {
    ++g_backend.follow_calls;
    if (g_backend.follow_target != 0) {
        if (out_followed != nullptr) *out_followed = true;
        return g_backend.follow_target;
    }
    return fake_resolve(symbol, out_followed);
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

HostDeps make_deps_with_follow() {
    return HostDeps{&fake_resolve, &fake_hook, &fake_unhook, &fake_resolve_follow};
}

// ---------------------------------------------------------------------------
// 指针槽（kPtrSlot）测试夹具：假原语直接写测试槽，站点经 test_stubs 的
// save_slot_delete_hook_target 可控返回。
// ---------------------------------------------------------------------------

void* g_ptr_slot_storage = nullptr;  // 模拟函数指针槽

bool fake_write_ptr(uintptr_t slot_addr, void* value) {
    *reinterpret_cast<void**>(slot_addr) = value;
    return true;
}

bool fake_restore_ptr(uintptr_t slot_addr, void* value) {
    *reinterpret_cast<void**>(slot_addr) = value;
    return true;
}

void use_fake_ptr_primitives() {
    qol::patch::set_primitives(qol::patch::Primitives{nullptr, nullptr, nullptr, nullptr,
                                                      &fake_write_ptr, &fake_restore_ptr});
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
    int ready_rc = 0;         // 非 0 = on_resource_ready 恒失败
    int ready_fail_call = 0;  // 第 N 次 on_resource_ready（1 起）失败；0 = 不用按次数失败
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
    if (g_probe.ready_fail_call != 0 && g_probe.ready_calls == g_probe.ready_fail_call) {
        return 1;
    }
    return g_probe.ready_rc;
}
void probe_fill_status(PluginStatusOut& out) {
    out.enabled = false;
    out.error = nullptr;
}

// kOwnership 探针：记录 acquire/release 调用与顺序（最多 8 条）。
struct OwnershipProbe {
    int acquire_calls = 0;
    int release_calls = 0;
    int fail_acquire_at = 0;      // 1-based；0 = 不失败
    int fail_acquire_code = -1;   // 失败时返回的原始码（默认 -1；NotReady 用例设 3）
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
    if (g_ownership.fail_acquire_at != 0 && call == g_ownership.fail_acquire_at) {
        return g_ownership.fail_acquire_code;
    }
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
    c.symbol = id;
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

ResourceClaim g_ok_resources[] = {
    claim("hook:test:ok:a", "test_ok", nullptr),
    claim("hook:test:ok:b", "test_ok", nullptr),
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
    qol::hooks::tx_reset_for_test();
    qol::infra::status_reset_for_test();
    qol::infra::capabilities_reset_for_test();
    qol::infra::config_reset_for_test();
    qol::infra::events_reset_for_test();
    g_backend = FakeBackend{};
    g_probe = PluginProbe{};
    g_ownership = OwnershipProbe{};
    g_ptr_slot_storage = nullptr;
    qol::game::g_ptr_slot_test_addr = nullptr;
    qol::game::g_ptr_slot_test_expected = 0;
    qol::game::g_ptr_slot_test_resolvable = false;
    qol::patch::claims_reset_for_test();
    use_fake_ptr_primitives();
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

void test_elf_export_prefix_rejected_for_other_mechanisms() {
    reset_all();
    ResourceClaim resource = ownership_claim("ownership:bad-export", "bad_export");
    resource.symbol = "elf-export:Fixture_Export_A";
    PluginDescriptor d = make_descriptor("bad_export", &resource, 1, nullptr, 0);
    CHECK(!register_plugin(d));
    CHECK(plugin_count() == 0);
}

// ---------------------------------------------------------------------------
// 生产 feature-mode 生命周期（真实 registry + 真实 hooks/transaction）
// ---------------------------------------------------------------------------

const qol::contracts::FeatureDefinition g_fm_features[] = {
    {"fm.test", 0, qol::contracts::FeatureCategory::kHidden,
     qol::contracts::FeatureType::kToggle, "fm", "frame-mode test", nullptr, nullptr},
};

PluginDescriptor make_feature_descriptor(const char* id, const ResourceClaim* resources,
                                         uint32_t resource_count) {
    PluginDescriptor d = make_descriptor(id, resources, resource_count, nullptr, 0);
    d.features = g_fm_features;
    d.feature_count = 1;
    return d;
}

qol::hooks::TxDeps::MechanismOutcome tx_install_other(const ResourceClaim& claim, uintptr_t) {
    if (claim.mechanism == ResourceMechanism::kOwnership) {
        const auto* ops = qol::contracts::ownership_ops(claim.handler);
        const int32_t rc = qol::contracts::ownership_acquire(ops, claim.id);
        return {rc, rc == 0 ? nullptr : "ownership acquire rejected"};
    }
    return {1, "mechanism unsupported"};
}

qol::hooks::TxDeps::MechanismOutcome tx_release_other(const ResourceClaim& claim, uintptr_t) {
    if (claim.mechanism == ResourceMechanism::kOwnership) {
        qol::contracts::ownership_release(qol::contracts::ownership_ops(claim.handler), claim.id);
    }
    return {0, nullptr};
}

void arm_tx_deps() {
    qol::hooks::tx_set_deps(
        qol::hooks::TxDeps{&fake_resolve, &fake_hook, &fake_unhook, nullptr, &tx_install_other,
                           &tx_release_other});
}

void arm_tx_deps_with_follow() {
    qol::hooks::tx_set_deps(
        qol::hooks::TxDeps{&fake_resolve, &fake_hook, &fake_unhook, nullptr, &tx_install_other,
                           &tx_release_other, &fake_resolve_follow});
}

void reset_feature_mode(const char* id, const ResourceClaim* resources, uint32_t count) {
    reset_all();
    arm_tx_deps();
    CHECK(register_plugin(make_feature_descriptor(id, resources, count)));
    CHECK(qol::host::feature_registry_prepare(make_deps()));
    qol::host::feature_registry_set_wanted(id, true);
}

void drive_feature_steps() {
    for (int i = 0; i < 400; ++i) {
        if (qol::host::feature_registry_step() == 0) break;
    }
}

ResourceClaim soft_claim(const char* id, const char* owner) {
    ResourceClaim c = claim(id, owner, nullptr);
    c.failure = ResourceFailure::kFailSoft;
    return c;
}

ResourceClaim fm_core_hooks[] = {
    claim("hook:fm:core:a", "fm_core", nullptr),
    claim("hook:fm:core:b", "fm_core", nullptr),
    claim("hook:fm:core:c", "fm_core", nullptr),
};

// 生产 feature-mode：核心 hook 逐条安装，任一 FailClosed 失败 → 逆序回滚已装子集 + Failed。
void test_feature_mode_core_hook_failure_rolls_back() {
    reset_feature_mode("fm_core", fm_core_hooks, 3);
    g_backend.fail_hook_call = 2;
    CHECK(qol::host::feature_registry_transition("fm_core", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_core", FeatureState::kFailed));
    CHECK(qol::hooks::state_of("hook:fm:core:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:fm:core:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:fm:core:c") == ResourceState::kFree);
    CHECK(g_backend.unhook_calls == 1);  // 逆序回滚第 1 条
    CHECK(g_probe.enabled_calls == 0);
    CHECK(!feature_degraded("fm_core"));  // 失败态不得误报 partial degraded
}

ResourceClaim fm_three_install[] = {
    claim("hook:fm:three:a", "fm_three", nullptr),
    claim("hook:fm:three:b", "fm_three", nullptr),
    claim("hook:fm:three:c", "fm_three", nullptr),
};

// 每帧多步安装：3 个资源远低于 kInstallStepsPerFrame（24），单次 feature_registry_step
// 内完成全部安装并进入 enabled 终态。
void test_feature_mode_installs_all_resources_in_one_step() {
    reset_feature_mode("fm_three", fm_three_install, 3);
    CHECK(qol::host::feature_registry_transition("fm_three", true));
    CHECK(qol::host::feature_registry_step() > 0);
    CHECK(feature_state_is("fm_three", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:fm:three:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:fm:three:b") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:fm:three:c") == ResourceState::kLive);
    CHECK(g_probe.enabled_calls == 1);
    // 成功终态不得留下瞬态重试标记。
    CHECK(!qol::host::feature_registry_pop_transient_failure("fm_three"));
}

ResourceClaim follow_hook_resource() {
    ResourceClaim c = claim("hook:follow:a", "follow_plugin", nullptr);
    c.follow_trampoline = true;
    return c;
}

// follow_trampoline opt-in：preflight 与安装都走 follow 解析，hook 落在跟随目标而非符号入口。
void test_follow_trampoline_uses_followed_entry() {
    ResourceClaim resources[] = {follow_hook_resource()};
    reset_all();
    arm_tx_deps_with_follow();
    g_backend.follow_target = 0xC000;
    CHECK(register_plugin(make_feature_descriptor("follow_plugin", resources, 1)));
    CHECK(qol::host::feature_registry_prepare(make_deps_with_follow()));
    qol::host::feature_registry_set_wanted("follow_plugin", true);
    CHECK(qol::host::feature_registry_transition("follow_plugin", true));
    drive_feature_steps();
    CHECK(feature_state_is("follow_plugin", FeatureState::kEnabled));
    CHECK(g_backend.follow_calls >= 1);
    CHECK(g_backend.last_target == reinterpret_cast<void*>(static_cast<uintptr_t>(0xC000)));
    CHECK(qol::hooks::state_of("hook:follow:a") == ResourceState::kLive);
}

ResourceClaim fm_soft_hooks[] = {
    soft_claim("hook:fm:soft:a", "fm_soft"),
    soft_claim("hook:fm:soft:b", "fm_soft"),
    soft_claim("hook:fm:soft:c", "fm_soft"),
};

// 生产 feature-mode：FailSoft 中段失败 → 该资源 Free、继续后续、事务成功 → enabled + degraded。
void test_feature_mode_soft_middle_keeps_degraded_reason() {
    reset_feature_mode("fm_soft", fm_soft_hooks, 3);
    g_backend.fail_hook_call = 2;
    CHECK(qol::host::feature_registry_transition("fm_soft", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_soft", FeatureState::kEnabled));
    CHECK(feature_degraded("fm_soft"));
    CHECK(qol::hooks::state_of("hook:fm:soft:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:fm:soft:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:fm:soft:c") == ResourceState::kLive);
    {
        const char* error = nullptr;
        CHECK(feature_of("fm_soft", nullptr, nullptr, &error));
        CHECK(error != nullptr && std::strstr(error, "hook:fm:soft:b") != nullptr);
    }
    // 成功终态不得清除软失败原因（g_gate_degraded 会被终态清掉，此项单独保留）。
    drive_feature_steps();
    CHECK(feature_degraded("fm_soft"));
    {
        const char* error = nullptr;
        CHECK(feature_of("fm_soft", nullptr, nullptr, &error));
        CHECK(error != nullptr && std::strstr(error, "hook:fm:soft:b") != nullptr);
    }
}

ResourceClaim fm_soft_last[] = {
    soft_claim("hook:fm:softlast:a", "fm_soft_last"),
    soft_claim("hook:fm:softlast:b", "fm_soft_last"),
};

// 生产 feature-mode：软失败发生在最后一条仍成功终态，末资源 Free + degraded。
void test_feature_mode_soft_last_keeps_degraded_reason() {
    reset_feature_mode("fm_soft_last", fm_soft_last, 2);
    g_backend.fail_hook_call = 2;
    CHECK(qol::host::feature_registry_transition("fm_soft_last", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_soft_last", FeatureState::kEnabled));
    CHECK(feature_degraded("fm_soft_last"));
    CHECK(qol::hooks::state_of("hook:fm:softlast:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:fm:softlast:b") == ResourceState::kFree);
    const char* error = nullptr;
    CHECK(feature_of("fm_soft_last", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "hook:fm:softlast:b") != nullptr);
}

ResourceClaim fm_soft_then_hard[] = {
    soft_claim("hook:fm:sh:soft", "fm_soft_hard"),
    claim("hook:fm:sh:hard", "fm_soft_hard", nullptr),
};

// 生产 feature-mode：软失败之后遇硬失败 → Failed；软失败资源保持 Free，未被伪装回滚。
void test_feature_mode_soft_then_hard_failure() {
    reset_feature_mode("fm_soft_hard", fm_soft_then_hard, 2);
    g_backend.unresolved_symbol = "hook:fm:sh:soft";
    g_backend.fail_hook_call = 1;
    CHECK(qol::host::feature_registry_transition("fm_soft_hard", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_soft_hard", FeatureState::kFailed));
    CHECK(qol::hooks::state_of("hook:fm:sh:soft") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:fm:sh:hard") == ResourceState::kFree);
    CHECK(g_backend.unhook_calls == 0);
    CHECK(g_probe.enabled_calls == 0);
}

ResourceClaim fm_ready_soft[] = {soft_claim("hook:fm:ready:soft", "fm_ready_soft")};

// 生产 feature-mode：on_resource_ready 失败且 claim 为 FailSoft → 安全撤回该资源为 Free，
// 仍 enabled + degraded，不置 Failed。
void test_feature_mode_ready_callback_soft_failure_degrades() {
    reset_feature_mode("fm_ready_soft", fm_ready_soft, 1);
    g_probe.ready_rc = 1;
    CHECK(qol::host::feature_registry_transition("fm_ready_soft", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_ready_soft", FeatureState::kEnabled));
    CHECK(feature_degraded("fm_ready_soft"));
    CHECK(qol::hooks::state_of("hook:fm:ready:soft") == ResourceState::kFree);
    CHECK(g_backend.unhook_calls == 1);  // 安全撤回
    CHECK(!qol::hooks::isolated_get("hook:fm:ready:soft"));
    const char* error = nullptr;
    CHECK(feature_of("fm_ready_soft", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "hook:fm:ready:soft") != nullptr);
}

ResourceClaim fm_ready_hard[] = {claim("hook:fm:ready:hard", "fm_ready_hard", nullptr)};

// 生产 feature-mode：FailClosed + ready 失败 → Failed（默认策略不变）。
// 末步 ready 失败时安装事务已终态：沿既有还原事务清理，资源回 Free 且最终 cleanup。
void test_feature_mode_ready_callback_hard_failure_fails() {
    reset_feature_mode("fm_ready_hard", fm_ready_hard, 1);
    g_probe.ready_rc = 1;
    CHECK(qol::host::feature_registry_transition("fm_ready_hard", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_ready_hard", FeatureState::kFailed));
    CHECK(g_probe.enabled_calls == 0);
    CHECK(qol::hooks::state_of("hook:fm:ready:hard") == ResourceState::kFree);
    CHECK(g_backend.unhook_calls == 1);  // 终态路径经还原事务释放
    CHECK(g_probe.deactivate_calls == 1);  // registrations 最终 cleanup
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
}

ResourceClaim fm_ownership_last[] = {
    claim("hook:fm:own:a", "fm_own_soft", nullptr),
    ownership_claim("own:fm:last", "fm_own_soft"),
};

// 生产 feature-mode：ownership 末条软失败 → 事务成功但 degraded、ownership Free。
void test_feature_mode_ownership_last_soft_failure() {
    fm_ownership_last[1].failure = ResourceFailure::kFailSoft;
    reset_feature_mode("fm_own_soft", fm_ownership_last, 2);
    g_ownership.fail_acquire_at = 1;
    CHECK(qol::host::feature_registry_transition("fm_own_soft", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_own_soft", FeatureState::kEnabled));
    CHECK(feature_degraded("fm_own_soft"));
    CHECK(qol::hooks::state_of("hook:fm:own:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("own:fm:last") == ResourceState::kFree);
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
    const char* error = nullptr;
    CHECK(feature_of("fm_own_soft", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "own:fm:last") != nullptr);
}

ResourceClaim fm_ready_abort[] = {
    claim("hook:fm:ra:a", "fm_ready_abort", nullptr),
    ownership_claim("own:fm:ra:b", "fm_ready_abort"),
};

// 生产 feature-mode：required 资源 ready 失败时安装事务仍在途 → 先中止安装，
// 禁止继续执行后续步（尤其末尾 ownership acquire），已装资源逆序释放并最终 cleanup。
void test_feature_mode_required_ready_failure_aborts_inflight_install() {
    fm_ready_abort[1].failure = ResourceFailure::kFailClosed;
    reset_feature_mode("fm_ready_abort", fm_ready_abort, 2);
    g_probe.ready_rc = 1;  // 第一步 ready 失败
    CHECK(qol::host::feature_registry_transition("fm_ready_abort", true));
    drive_feature_steps();

    CHECK(feature_state_is("fm_ready_abort", FeatureState::kFailed));
    CHECK(g_ownership.acquire_calls == 0);  // 末尾 ownership 从未被尝试
    CHECK(qol::hooks::state_of("hook:fm:ra:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("own:fm:ra:b") == ResourceState::kFree);
    CHECK(g_backend.unhook_calls == 1);  // 已装 A 逆序释放一次
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
    CHECK(g_probe.enabled_calls == 0);
    CHECK(g_probe.deactivate_calls == 1);  // 最终 failed cleanup
}

ResourceClaim fm_soft_ready_then_hard[] = {
    soft_claim("hook:fm:srh:a", "fm_srh"),
    claim("hook:fm:srh:b", "fm_srh", nullptr),
};

// 生产 feature-mode：软 ready 失败安全撤回后，后续 required 安装硬失败 →
// 已撤回资源保持 Free、不 isolated、不二次 unhook；后续实际安装的资源仍逆序释放。
void test_feature_mode_soft_ready_revoke_then_hard_failure() {
    reset_feature_mode("fm_srh", fm_soft_ready_then_hard, 2);
    g_probe.ready_fail_call = 1;  // 第一条（软）ready 失败
    g_backend.fail_hook_call = 2;  // 第二条安装失败
    CHECK(qol::host::feature_registry_transition("fm_srh", true));
    drive_feature_steps();

    CHECK(feature_state_is("fm_srh", FeatureState::kFailed));
    CHECK(qol::hooks::state_of("hook:fm:srh:a") == ResourceState::kFree);
    CHECK(!qol::hooks::isolated_get("hook:fm:srh:a"));
    CHECK(qol::hooks::state_of("hook:fm:srh:b") == ResourceState::kFree);
    CHECK(g_backend.unhook_calls == 1);  // 仅安全撤回一次，无二次 unhook
    CHECK(g_probe.enabled_calls == 0);
}

ResourceClaim fm_release_inflight[] = {claim("hook:fm:rel:a", "fm_release", nullptr)};

// 生产 feature-mode：撤销等待在途归零；在途未清前不得进入 inactive。
void test_feature_mode_release_waits_for_in_flight() {
    reset_feature_mode("fm_release", fm_release_inflight, 1);
    CHECK(qol::host::feature_registry_transition("fm_release", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_release", FeatureState::kEnabled));
    CHECK(qol::hooks::state_of("hook:fm:rel:a") == ResourceState::kLive);

    CHECK(qol::hooks::in_flight_enter("hook:fm:rel:a"));
    CHECK(qol::host::feature_registry_transition("fm_release", false));
    drive_feature_steps();
    CHECK(!feature_state_is("fm_release", FeatureState::kInactive));
    // 在途未归零：资源仍物理安装（live 或 stop 预备后的 disabled），不得提前 kFree。
    const ResourceState held = qol::hooks::state_of("hook:fm:rel:a");
    CHECK(held == ResourceState::kLive || held == ResourceState::kDisabled);

    qol::hooks::in_flight_leave("hook:fm:rel:a");
    drive_feature_steps();
    CHECK(feature_state_is("fm_release", FeatureState::kInactive));
}

// ---------------------------------------------------------------------------
// 启动期 ownership（表注入）NotReady 的 SaveEnter 定向恢复
// ---------------------------------------------------------------------------

ResourceClaim fm_startup_res[] = {
    claim("hook:fm:su:a", "fm_startup", nullptr),
    ownership_claim("own:fm:su:b", "fm_startup"),
};
ResourceClaim fm_startup_other[] = {claim("hook:fm:su:other", "fm_startup_other", nullptr)};

// 安装一条 hook + 一条 ownership（fail-soft），并让 ownership 以 NotReady 失败。
// 返回时安装已终态：hook live、ownership free + 武装。
void setup_startup_ownership_notready(const char* owner, int32_t code = kQolErrNotReady) {
    reset_all();
    arm_tx_deps();
    CHECK(register_plugin(make_feature_descriptor(owner, fm_startup_res, 2)));
    CHECK(qol::host::feature_registry_prepare(make_deps()));
    qol::host::feature_registry_set_wanted(owner, true);
    g_ownership.fail_acquire_at = 1;
    g_ownership.fail_acquire_code = code;
    CHECK(qol::host::feature_registry_transition(owner, true));
    drive_feature_steps();
}

// 终态：ownership 保持 Free（不伪装 Live）、其余资源正常安装、武装一次恢复。
void test_startup_ownership_notready_free_and_other_owner_continues() {
    fm_startup_res[1].failure = ResourceFailure::kFailSoft;
    reset_all();
    arm_tx_deps();
    CHECK(register_plugin(make_feature_descriptor("fm_startup", fm_startup_res, 2)));
    CHECK(register_plugin(make_feature_descriptor("fm_startup_other", fm_startup_other, 1)));
    CHECK(qol::host::feature_registry_prepare(make_deps()));
    qol::host::feature_registry_set_wanted("fm_startup", true);
    qol::host::feature_registry_set_wanted("fm_startup_other", true);
    g_ownership.fail_acquire_at = 1;
    g_ownership.fail_acquire_code = kQolErrNotReady;
    CHECK(qol::host::feature_registry_transition("fm_startup", true));
    CHECK(qol::host::feature_registry_transition("fm_startup_other", true));
    drive_feature_steps();

    CHECK(feature_state_is("fm_startup", FeatureState::kEnabled));
    CHECK(feature_state_is("fm_startup_other", FeatureState::kEnabled));  // 其他 owner 继续装配
    CHECK(qol::hooks::state_of("hook:fm:su:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("own:fm:su:b") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("own:fm:su:b"));
    CHECK(feature_degraded("fm_startup"));
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
    {
        const char* error = nullptr;
        CHECK(feature_of("fm_startup", nullptr, nullptr, &error));
        CHECK(error != nullptr && std::strstr(error, "own:fm:su:b") != nullptr);
    }
}

// 无 SaveEnter 信号：不重试（不逐帧、不定时）。
void test_startup_ownership_no_signal_no_retry() {
    fm_startup_res[1].failure = ResourceFailure::kFailSoft;
    setup_startup_ownership_notready("fm_startup");
    const int acquires = g_ownership.acquire_calls;
    for (int i = 0; i < 5; ++i) drive_feature_steps();
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 0);
    CHECK(g_ownership.acquire_calls == acquires);
    CHECK(qol::hooks::state_of("own:fm:su:b") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("own:fm:su:b"));
}

// SaveEnter 后单 claim 恢复成功：仅 ownership 变化，12 hooks 不重装；degraded/error 清除。
void test_startup_ownership_save_enter_restores_single_claim() {
    fm_startup_res[1].failure = ResourceFailure::kFailSoft;
    setup_startup_ownership_notready("fm_startup");
    const int hook_calls = g_backend.hook_calls;
    const int acquires = g_ownership.acquire_calls;
    const int ready_calls = g_probe.ready_calls;

    g_ownership.fail_acquire_at = 0;  // 表已就绪
    qol::host::feature_registry_notify_save_ready();
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 1);

    CHECK(g_ownership.acquire_calls == acquires + 1);
    CHECK(qol::hooks::state_of("own:fm:su:b") == ResourceState::kLive);
    CHECK(!qol::hooks::retry_pending_get("own:fm:su:b"));
    CHECK(g_backend.hook_calls == hook_calls);  // 不重装 hooks
    CHECK(g_probe.ready_calls == ready_calls + 1);
    CHECK(std::strcmp(g_probe.last_ready_id, "own:fm:su:b") == 0);
    CHECK(!feature_degraded("fm_startup"));
    {
        const char* error = nullptr;
        CHECK(feature_of("fm_startup", nullptr, nullptr, &error));
        CHECK(error == nullptr);
    }
}

// 生产帧序模拟：每帧先推进生命周期事务，再消费 SaveEnter 信号（同 assembly
// drain_commands_task: production_feature_frame_step → feature_registry_retry_pending_ownership）。
// 返回各帧实际恢复尝试次数之和。
int run_production_frames(int frames) {
    int attempts = 0;
    for (int f = 0; f < frames; ++f) {
        qol::host::feature_registry_step();
        attempts += static_cast<int>(qol::host::feature_registry_retry_pending_ownership());
    }
    return attempts;
}

// 信号在 ownership 末步之前（安装事务进行中）到达：按生产帧序不得在安装推进中途被消费；
// 每帧多步安装使该帧走完终态后同帧消费，恰好恢复一次；hooks 不重装；无新信号时不再尝试。
void test_startup_ownership_signal_before_final_step_restores_once() {
    fm_startup_res[1].failure = ResourceFailure::kFailSoft;
    reset_all();
    arm_tx_deps();
    CHECK(register_plugin(make_feature_descriptor("fm_startup", fm_startup_res, 2)));
    CHECK(qol::host::feature_registry_prepare(make_deps()));
    qol::host::feature_registry_set_wanted("fm_startup", true);
    g_ownership.fail_acquire_at = 1;
    g_ownership.fail_acquire_code = kQolErrNotReady;
    CHECK(qol::host::feature_registry_transition("fm_startup", true));

    // SaveEnter 在安装事务仍在途（ownership 末步尚未失败）时锁存。
    qol::host::feature_registry_notify_save_ready();
    // 2 个资源在单帧内走完安装终态；信号在该帧终态后被消费 → 恰好恢复一次。
    CHECK(run_production_frames(1) == 1);
    CHECK(qol::hooks::state_of("own:fm:su:b") == ResourceState::kLive);
    CHECK(!qol::hooks::retry_pending_get("own:fm:su:b"));
    CHECK(g_ownership.acquire_calls == 2);  // 初始 NotReady 1 次 + 恢复 1 次
    CHECK(feature_state_is("fm_startup", FeatureState::kEnabled));
    const int hook_calls_during_install = g_backend.hook_calls;

    // 无新信号：后续帧不再尝试（不逐帧重试），hooks 不重装。
    CHECK(run_production_frames(3) == 0);
    CHECK(g_ownership.acquire_calls == 2);
    CHECK(g_backend.hook_calls == hook_calls_during_install);
}

// 每信号仅一次：同帧重复/重复帧不再尝试；claim 已 Live 后新信号无目标。
void test_startup_ownership_signal_consumed_once() {
    fm_startup_res[1].failure = ResourceFailure::kFailSoft;
    setup_startup_ownership_notready("fm_startup");
    g_ownership.fail_acquire_at = 0;
    const int acquires = g_ownership.acquire_calls;
    qol::host::feature_registry_notify_save_ready();
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 1);
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 0);  // 重复帧
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 0);
    CHECK(g_ownership.acquire_calls == acquires + 1);
    qol::host::feature_registry_notify_save_ready();  // 新信号但已 Live → 无目标
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 0);
    CHECK(g_ownership.acquire_calls == acquires + 1);
}

// 永久错误（非 NotReady）不武装；即使收到 SaveEnter 也不尝试。
void test_startup_ownership_permanent_error_not_armed() {
    fm_startup_res[1].failure = ResourceFailure::kFailSoft;
    setup_startup_ownership_notready("fm_startup", kQolErrUnsupported);
    CHECK(!qol::hooks::retry_pending_get("own:fm:su:b"));
    const int acquires = g_ownership.acquire_calls;
    qol::host::feature_registry_notify_save_ready();
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 0);
    CHECK(g_ownership.acquire_calls == acquires);
    CHECK(qol::hooks::state_of("own:fm:su:b") == ResourceState::kFree);
}

// 关闭/撤销清信号与 pending；旧 token 被拒；重新激活按新 token 重新武装。
void test_startup_ownership_stop_clears_and_old_token_rejected() {
    fm_startup_res[1].failure = ResourceFailure::kFailSoft;
    setup_startup_ownership_notready("fm_startup");
    CHECK(qol::host::feature_registry_transition("fm_startup", false));
    drive_feature_steps();
    CHECK(feature_state_is("fm_startup", FeatureState::kInactive));
    CHECK(!qol::hooks::retry_pending_get("own:fm:su:b"));

    const int acquires = g_ownership.acquire_calls;
    qol::host::feature_registry_notify_save_ready();  // 旧 token 已失效
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 0);
    CHECK(g_ownership.acquire_calls == acquires);

    // desired 关闭时也拒绝恢复。
    g_ownership.fail_acquire_at = g_ownership.acquire_calls + 1;
    g_ownership.fail_acquire_code = kQolErrNotReady;
    CHECK(qol::host::feature_registry_transition("fm_startup", true));
    drive_feature_steps();
    CHECK(qol::hooks::retry_pending_get("own:fm:su:b"));
    qol::host::feature_registry_set_wanted("fm_startup", false);
    qol::host::feature_registry_notify_save_ready();
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 0);
    CHECK(qol::hooks::state_of("own:fm:su:b") == ResourceState::kFree);
}

// ready 通知失败：按事实撤回到 Free（或隔离），不留下 Live 假象。
void test_startup_ownership_ready_notify_failure_revokes() {
    fm_startup_res[1].failure = ResourceFailure::kFailSoft;
    setup_startup_ownership_notready("fm_startup");
    g_ownership.fail_acquire_at = 0;
    g_probe.ready_rc = 1;  // 恢复通知失败
    qol::host::feature_registry_notify_save_ready();
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 1);
    CHECK(qol::hooks::state_of("own:fm:su:b") == ResourceState::kFree);
    CHECK(!qol::hooks::isolated_get("own:fm:su:b"));
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
    CHECK(feature_degraded("fm_startup"));
    // error 必须指向事实失败资源并带原因（HTTP 不得出现 degraded 无原因）。
    {
        const char* error = nullptr;
        CHECK(feature_of("fm_startup", nullptr, nullptr, &error));
        CHECK(error != nullptr && std::strstr(error, "own:fm:su:b") != nullptr);
        CHECK(error != nullptr && std::strstr(error, "resource_ready_failed") != nullptr);
    }
}

ResourceClaim fm_startup_opt_res[] = {
    optional_claim("hook:fm:su:opt", "fm_startup_opt"),
    ownership_claim("own:fm:su:opt", "fm_startup_opt"),
};

// 其他 optional 仍失败（保持 Free）时，ownership 恢复成功不得清掉 degraded。
void test_startup_ownership_restore_keeps_other_optional_degraded() {
    fm_startup_opt_res[0].requirement = ResourceRequirement::kOptional;
    fm_startup_opt_res[0].failure = ResourceFailure::kFailSoft;
    fm_startup_opt_res[1].failure = ResourceFailure::kFailSoft;
    reset_all();
    arm_tx_deps();
    CHECK(register_plugin(make_feature_descriptor("fm_startup_opt", fm_startup_opt_res, 2)));
    CHECK(qol::host::feature_registry_prepare(make_deps()));
    qol::host::feature_registry_set_wanted("fm_startup_opt", true);
    g_backend.fail_hook_call = 1;  // optional hook 安装失败
    g_ownership.fail_acquire_at = 1;
    g_ownership.fail_acquire_code = kQolErrNotReady;
    CHECK(qol::host::feature_registry_transition("fm_startup_opt", true));
    drive_feature_steps();
    CHECK(qol::hooks::state_of("hook:fm:su:opt") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("own:fm:su:opt") == ResourceState::kFree);
    CHECK(feature_degraded("fm_startup_opt"));

    g_ownership.fail_acquire_at = 0;
    qol::host::feature_registry_notify_save_ready();
    CHECK(qol::host::feature_registry_retry_pending_ownership() == 1);
    CHECK(qol::hooks::state_of("own:fm:su:opt") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:fm:su:opt") == ResourceState::kFree);
    CHECK(feature_degraded("fm_startup_opt"));  // 其他 optional 失败仍保持 degraded
    // error 必须改指实际仍未安装的 optional 资源，而不是已 Live 的 ownership。
    {
        const char* error = nullptr;
        CHECK(feature_of("fm_startup_opt", nullptr, nullptr, &error));
        CHECK(error != nullptr && std::strstr(error, "hook:fm:su:opt") != nullptr);
        CHECK(error != nullptr && std::strstr(error, "own:fm:su:opt") == nullptr);
    }
}

// 最近失败码：软失败步的 NotReady 保留到该插件事务成功终态，随后清零；非法 id 返回 0。
void test_last_failure_code_tracks_notready_until_success() {
    static char ids[26][32];
    static ResourceClaim claims[26];
    for (int i = 0; i < 26; ++i) {
        std::snprintf(ids[i], sizeof(ids[i]), "own:fm:code:%02d", i);
        claims[i] = ownership_claim(ids[i], "fm_code");
    }
    // required + fail-soft：安装步软失败保留原始机制码（NotReady），事务继续推进。
    claims[0].failure = ResourceFailure::kFailSoft;
    reset_all();
    arm_tx_deps();
    CHECK(register_plugin(make_feature_descriptor("fm_code", claims, 26)));
    CHECK(qol::host::feature_registry_prepare(make_deps()));
    qol::host::feature_registry_set_wanted("fm_code", true);
    CHECK(qol::host::feature_registry_last_failure_code(nullptr) == 0);
    CHECK(qol::host::feature_registry_last_failure_code("not.registered") == 0);

    g_ownership.fail_acquire_at = 1;
    g_ownership.fail_acquire_code = kQolErrNotReady;
    CHECK(qol::host::feature_registry_transition("fm_code", true));
    // 资源数超过 kInstallStepsPerFrame：首帧 24 步后事务仍在途，NotReady 记录可观察。
    CHECK(qol::host::feature_registry_step() > 0);
    CHECK(qol::host::feature_registry_last_failure_code("fm_code") == kQolErrNotReady);
    drive_feature_steps();
    // 事务成功终态：记录清零（其余资源已安装，插件按 degraded 保持可用）。
    CHECK(qol::host::feature_registry_last_failure_code("fm_code") == 0);
    g_ownership.fail_acquire_at = 0;
}

ResourceClaim fm_code_required[] = {ownership_claim("own:fm:code:req", "fm_code_req")};

// required + fail-closed 安装失败终态也必须保留原始机制码：NotReady 是 Reconciler
// 判断可重试的唯一依据，不能退化成通用失败码 1。
void test_last_failure_code_keeps_notready_on_failed_terminal() {
    reset_feature_mode("fm_code_req", fm_code_required, 1);
    g_ownership.fail_acquire_at = 1;
    g_ownership.fail_acquire_code = kQolErrNotReady;
    CHECK(qol::host::feature_registry_transition("fm_code_req", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_code_req", FeatureState::kFailed));
    CHECK(qol::hooks::state_of("own:fm:code:req") == ResourceState::kFree);
    CHECK(qol::host::feature_registry_last_failure_code("fm_code_req") == kQolErrNotReady);
    // NotReady 失败发生在 step 内部：一次性标记供 Reconciler 在 step 后消费。
    CHECK(qol::host::feature_registry_pop_transient_failure("fm_code_req"));
    CHECK(!qol::host::feature_registry_pop_transient_failure("fm_code_req"));
    CHECK(!qol::host::feature_registry_pop_transient_failure("not.registered"));
}

ResourceClaim fm_code_permanent[] = {claim("hook:fm:code:perm:a", "fm_code_perm", nullptr)};

// 永久失败（安装原语返回普通错误）不得被误报为 NotReady，否则会触发无意义重试。
void test_last_failure_code_permanent_error_is_generic() {
    reset_feature_mode("fm_code_perm", fm_code_permanent, 1);
    g_backend.fail_hook_call = 1;
    CHECK(qol::host::feature_registry_transition("fm_code_perm", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_code_perm", FeatureState::kFailed));
    CHECK(qol::host::feature_registry_last_failure_code("fm_code_perm") == 1);
    CHECK(qol::host::feature_registry_last_failure_code("fm_code_perm") != kQolErrNotReady);
}

static int g_dup_iface_a = 0;
static int g_dup_iface_b = 0;
const qol::contracts::CapabilityProvisionRef g_dup_provides_a[] = {
    {"test.dup.cap", {1, 0}, &g_dup_iface_a}};
const qol::contracts::CapabilityProvisionRef g_dup_provides_b[] = {
    {"test.dup.cap", {1, 0}, &g_dup_iface_b}};
const qol::contracts::CapabilityRequirement g_dup_requires[] = {{"test.dup.cap", {1, 0}}};

// 插件 handler 抛异常：能力变更事件在 provider 事务终态同步派发，必须被隔离，
// 不能穿透生命周期引擎。
void throwing_capability_handler(void*, const void*) { throw 7; }

// 两个插件声明同一能力 id：第二个注册失败不得进入 kRegistered，也不得在停用时
// 注销第一个 provider 的真实能力。
void test_duplicate_capability_provider_ownership() {
    reset_all();
    g_log_capture.clear();
    arm_tx_deps();
    PluginDescriptor first = make_feature_descriptor("fm_dup_a", nullptr, 0);
    first.provides = g_dup_provides_a;
    first.provides_count = 1;
    PluginDescriptor second = make_feature_descriptor("fm_dup_b", nullptr, 0);
    second.provides = g_dup_provides_b;
    second.provides_count = 1;
    PluginDescriptor consumer = make_feature_descriptor("fm_dup_consumer", nullptr, 0);
    consumer.requires = g_dup_requires;
    consumer.requires_count = 1;
    CHECK(register_plugin(first));
    CHECK(register_plugin(second));
    CHECK(register_plugin(consumer));
    CHECK(qol::host::feature_registry_prepare(make_deps()));
    qol::host::feature_registry_set_wanted("fm_dup_a", true);
    qol::host::feature_registry_set_wanted("fm_dup_b", true);
    const auto sub = qol::infra::subscribe(qol::contracts::kEventCapabilityChanged,
                                           &throwing_capability_handler, nullptr);
    CHECK(sub != 0);

    CHECK(qol::host::feature_registry_transition("fm_dup_a", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_dup_a", FeatureState::kEnabled));
    CHECK(qol::infra::require_capability("test.dup.cap", {1, 0}) ==
          static_cast<const void*>(&g_dup_iface_a));
    CHECK(g_log_capture.find("capability event handler threw") != std::string::npos);

    CHECK(qol::host::feature_registry_transition("fm_dup_b", true));
    drive_feature_steps();
    CHECK(feature_state_is("fm_dup_b", FeatureState::kEnabled));
    // 第二个 provider 同名能力注册失败：不劫持 require，并允许下次 enable 重试。
    CHECK(qol::infra::require_capability("test.dup.cap", {1, 0}) ==
          static_cast<const void*>(&g_dup_iface_a));
    CHECK(g_log_capture.find("capability registration failed") != std::string::npos);

    // 注销第二个 provider 不删除第一个的能力；消费者仍可 require。
    CHECK(qol::host::feature_registry_transition("fm_dup_b", false));
    drive_feature_steps();
    CHECK(feature_state_is("fm_dup_b", FeatureState::kInactive));
    CHECK(qol::infra::require_capability("test.dup.cap", {1, 0}) ==
          static_cast<const void*>(&g_dup_iface_a));
    CHECK(qol::infra::unsubscribe(sub));
}

}  // namespace

int main() {
    qol_log_set_host_sink(&capture_log);
    test_duplicate_and_invalid_registration();
    test_reset_is_clean();
    test_elf_export_prefix_rejected_for_other_mechanisms();
    test_feature_mode_core_hook_failure_rolls_back();
    test_feature_mode_installs_all_resources_in_one_step();
    test_follow_trampoline_uses_followed_entry();
    test_feature_mode_soft_middle_keeps_degraded_reason();
    test_feature_mode_soft_last_keeps_degraded_reason();
    test_feature_mode_soft_then_hard_failure();
    test_feature_mode_ready_callback_soft_failure_degrades();
    test_feature_mode_ready_callback_hard_failure_fails();
    test_feature_mode_required_ready_failure_aborts_inflight_install();
    test_feature_mode_soft_ready_revoke_then_hard_failure();
    test_feature_mode_ownership_last_soft_failure();
    test_feature_mode_release_waits_for_in_flight();
    test_startup_ownership_notready_free_and_other_owner_continues();
    test_startup_ownership_no_signal_no_retry();
    test_startup_ownership_save_enter_restores_single_claim();
    test_startup_ownership_signal_before_final_step_restores_once();
    test_startup_ownership_signal_consumed_once();
    test_startup_ownership_permanent_error_not_armed();
    test_startup_ownership_stop_clears_and_old_token_rejected();
    test_startup_ownership_ready_notify_failure_revokes();
    test_startup_ownership_restore_keeps_other_optional_degraded();
    test_last_failure_code_tracks_notready_until_success();
    test_last_failure_code_keeps_notready_on_failed_terminal();
    test_last_failure_code_permanent_error_is_generic();
    test_duplicate_capability_provider_ownership();
    std::printf("plugin_registry: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
