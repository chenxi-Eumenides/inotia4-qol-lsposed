// hooks/registry.h + hooks/install.h + hooks/transaction.h host 单测：
// 覆盖资源运行期记录/在途计数/隔离标记/物理释放，以及帧步进事务的安装顺序、
// 失败回滚、不可还原、在途等待与超预算隔离、事务查询与保留淘汰。

#include "hooks/install.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"

#include "contracts/ownership.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::contracts::ResourceClaim;
using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourcePolicy;
using qol::contracts::ResourceRequirement;
using qol::contracts::ResourceState;
using qol::hooks::install_claim_entry_hook;
using qol::hooks::InstallOutcome;
using qol::hooks::in_flight_count;
using qol::hooks::in_flight_enter;
using qol::hooks::in_flight_leave;
using qol::hooks::InFlightGuard;
using qol::hooks::isolated_get;
using qol::hooks::isolated_set;
using qol::contracts::resource_state_name;
using qol::hooks::register_claim;
using qol::hooks::registry_reset_for_test;
using qol::hooks::runtime_clear;
using qol::hooks::runtime_set;
using qol::hooks::release_claim_entry_hook;
using qol::hooks::ReleaseOutcome;
using qol::hooks::runtime_of;
using qol::hooks::set_state;
using qol::hooks::state_of;
using qol::hooks::TxDeps;
using qol::hooks::TxKind;
using qol::hooks::TxState;
using qol::hooks::TxStatus;
using qol::hooks::TxStep;
using qol::hooks::tx_abort_install;
using qol::hooks::tx_begin;
using qol::hooks::tx_query;
using qol::hooks::tx_reset_for_test;
using qol::hooks::tx_set_deps;
using qol::hooks::tx_step;
using qol::hooks::ResourceRuntime;

namespace {

int g_handler_a = 0;
int g_handler_b = 0;

// 假机制原语全局状态。
uintptr_t g_resolve_value = 0x1000;
int g_resolve_calls = 0;
const char* g_alias_symbol_a = nullptr;
const char* g_alias_symbol_b = nullptr;
uintptr_t g_alias_target = 0;
const char* g_resolved_symbols[128] = {};
uint32_t g_resolved_symbol_count = 0;
int g_hook_calls = 0;
int g_hook_fail_at = -1;  // 1-based；-1 = 永不失败
int g_hook_no_backup_at = -1;  // 1-based；该次成功返回 0 但不写 backup
void* g_hook_targets[16] = {};
void* g_hook_replacements[16] = {};
int g_unhook_calls = 0;
int g_unhook_fail_at = -1;  // 1-based；-1 = 永不失败
void* g_last_unhook_arg = nullptr;
int g_gate_calls = 0;

uintptr_t fake_resolve(const char* symbol, bool* out_followed) {
    ++g_resolve_calls;
    if (out_followed != nullptr) {
        *out_followed = false;
    }
    if (g_resolve_value == 0 || symbol == nullptr) return 0;
    if (g_alias_target != 0 &&
        ((g_alias_symbol_a != nullptr && std::strcmp(symbol, g_alias_symbol_a) == 0) ||
         (g_alias_symbol_b != nullptr && std::strcmp(symbol, g_alias_symbol_b) == 0))) {
        return g_alias_target;
    }
    for (uint32_t i = 0; i < g_resolved_symbol_count; ++i) {
        if (std::strcmp(symbol, g_resolved_symbols[i]) == 0) {
            return g_resolve_value + static_cast<uintptr_t>(i) * 0x100;
        }
    }
    if (g_resolved_symbol_count >= 128) return 0;
    const uint32_t slot = g_resolved_symbol_count++;
    g_resolved_symbols[slot] = symbol;
    return g_resolve_value + static_cast<uintptr_t>(slot) * 0x100;
}

int fake_hook(void* target, void* replacement, void** backup) {
    const int call = g_hook_calls + 1;
    ++g_hook_calls;
    if (call < 16) {
        g_hook_targets[call] = target;
        g_hook_replacements[call] = replacement;
    }
    if (g_hook_fail_at > 0 && call == g_hook_fail_at) {
        return -1;
    }
    if (g_hook_no_backup_at > 0 && call == g_hook_no_backup_at) {
        return 0;  // 成功但不写 backup
    }
    if (backup != nullptr) {
        *backup = reinterpret_cast<void*>(0x2000 + call);
    }
    return 0;
}

int fake_unhook(void* func) {
    g_last_unhook_arg = func;
    const int call = ++g_unhook_calls;
    if (g_unhook_fail_at > 0 && call == g_unhook_fail_at) {
        return -1;
    }
    return 0;
}

// gate 键 "gateOff" 关闭，其余视为开。
bool fake_gate(const char* key) {
    ++g_gate_calls;
    return key == nullptr || std::strcmp(key, "gateOff") != 0;
}

// 非入口机制分派（kCallPatch）：记录调用与目标，可注入失败。
using MechanismOutcome = TxDeps::MechanismOutcome;
int g_other_install_calls = 0;
int g_other_release_calls = 0;
int g_other_install_rc = 0;
int g_other_release_rc = 0;
uintptr_t g_other_last_target = 0;
const ResourceClaim* g_other_last_claim = nullptr;
// 机制层交还的原函数入口（kPtrSlot 语义）；默认 null = 无原值的机制。
void* g_other_install_backup = nullptr;

MechanismOutcome fake_install_other(const ResourceClaim& claim, uintptr_t target) {
    ++g_other_install_calls;
    g_other_last_claim = &claim;
    g_other_last_target = target;
    if (g_other_install_rc != 0) {
        // 原始机制码原样返回（host 依赖 NotReady 等具体码做定向恢复）。
        return MechanismOutcome{g_other_install_rc, "primitive rejected"};
    }
    return MechanismOutcome{0, nullptr, g_other_install_backup};
}

MechanismOutcome fake_release_other(const ResourceClaim& claim, uintptr_t target) {
    ++g_other_release_calls;
    g_other_last_claim = &claim;
    g_other_last_target = target;
    if (g_other_release_rc != 0) {
        return MechanismOutcome{1, "primitive rejected"};
    }
    return MechanismOutcome{0, nullptr};
}

// kOwnership：模拟宿主分派（assembly 的 dispatch_install/release）——调 claim.handler
// 指向的 OwnershipOps。事务引擎只按 mechanism 选择 install_other/release_other，
// 资源顺序与还原逆序由事务层保证。
int g_own_acquire_calls = 0;
int g_own_release_calls = 0;
char g_own_acquire_order[8][32] = {};
char g_own_release_order[8][32] = {};

int32_t probe_ownership_acquire(const char* id) {
    const int call = ++g_own_acquire_calls;
    if (id != nullptr && call <= 8) {
        std::snprintf(g_own_acquire_order[call - 1], sizeof(g_own_acquire_order[0]), "%s", id);
    }
    return 0;
}

void probe_ownership_release(const char* id) {
    const int call = ++g_own_release_calls;
    if (id != nullptr && call <= 8) {
        std::snprintf(g_own_release_order[call - 1], sizeof(g_own_release_order[0]), "%s", id);
    }
}

const qol::contracts::OwnershipOps g_probe_ownership_ops = {&probe_ownership_acquire,
                                                            &probe_ownership_release};

MechanismOutcome ownership_install_other(const ResourceClaim& claim, uintptr_t target) {
    (void)target;
    const int32_t rc =
        qol::contracts::ownership_acquire(qol::contracts::ownership_ops(claim.handler), claim.id);
    return MechanismOutcome{rc, rc == 0 ? nullptr : "ownership acquire rejected"};
}

MechanismOutcome ownership_release_other(const ResourceClaim& claim, uintptr_t target) {
    (void)target;
    qol::contracts::ownership_release(qol::contracts::ownership_ops(claim.handler), claim.id);
    return MechanismOutcome{0, nullptr};
}

void reset_all() {
    registry_reset_for_test();
    tx_reset_for_test();
    g_resolve_value = 0x1000;
    g_resolve_calls = 0;
    g_alias_symbol_a = nullptr;
    g_alias_symbol_b = nullptr;
    g_alias_target = 0;
    std::memset(g_resolved_symbols, 0, sizeof(g_resolved_symbols));
    g_resolved_symbol_count = 0;
    g_hook_calls = 0;
    g_hook_fail_at = -1;
    g_hook_no_backup_at = -1;
    g_unhook_calls = 0;
    g_unhook_fail_at = -1;
    g_last_unhook_arg = nullptr;
    g_gate_calls = 0;
    g_other_install_calls = 0;
    g_other_release_calls = 0;
    g_other_install_rc = 0;
    g_other_release_rc = 0;
    g_other_last_target = 0;
    g_other_last_claim = nullptr;
    g_other_install_backup = nullptr;
    g_own_acquire_calls = 0;
    g_own_release_calls = 0;
    std::memset(g_own_acquire_order, 0, sizeof(g_own_acquire_order));
    std::memset(g_own_release_order, 0, sizeof(g_own_release_order));
    std::memset(g_hook_targets, 0, sizeof(g_hook_targets));
    std::memset(g_hook_replacements, 0, sizeof(g_hook_replacements));
}

void set_deps() {
    TxDeps deps{};
    deps.resolve_entry = fake_resolve;
    deps.hook_fn = fake_hook;
    deps.unhook_fn = fake_unhook;
    deps.gate_on = fake_gate;
    deps.install_other = fake_install_other;
    deps.release_other = fake_release_other;
    tx_set_deps(deps);
}

ResourceClaim make_claim(const char* id, const char* owner, const char* gate = "gateOn",
                         bool resumable = true, uint32_t step_budget = 120,
                         void* handler = &g_handler_a) {
    ResourceClaim claim{};
    claim.id = id;
    claim.symbol = id;
    claim.mechanism = ResourceMechanism::kEntryHook;
    claim.policy = ResourcePolicy::kPrivate;
    claim.requirement = ResourceRequirement::kRequired;
    claim.failure = ResourceFailure::kFailClosed;
    claim.resumable = resumable;
    claim.owner = owner;
    claim.reason = "test";
    claim.gate = gate;
    claim.step_budget = step_budget;
    claim.handler = handler;
    return claim;
}

// 安装一个资源并断言成功（供还原用例构造 kLive 前置状态）。
uint64_t install_one(const char* plugin_id) {
    const uint64_t tx = tx_begin(plugin_id, TxKind::kInstall);
    TxStep step{};
    const bool more = tx_step(tx, &step);
    CHECK(!more);
    CHECK(step.finished);
    return tx;
}

void test_install_success_sequence() {
    reset_all();
    set_deps();
    const ResourceClaim a = make_claim("hook:A", "plugin_a", "gateOn", true, 120, &g_handler_a);
    const ResourceClaim b = make_claim("hook:B", "plugin_a", "gateOff", true, 120, &g_handler_b);
    CHECK(register_claim(a));
    CHECK(register_claim(b));

    const uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
    CHECK(tx != 0);
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kRunning);
    CHECK(status.total == 2);
    CHECK(status.step == 0);
    CHECK(status.frames == 0);
    CHECK(status.kind == TxKind::kInstall);

    TxStep first{};
    CHECK(tx_step(tx, &first));
    CHECK(std::strcmp(first.resource_id, "hook:A") == 0);
    CHECK(first.ok == 0);
    CHECK(!first.finished);
    CHECK(first.backup == reinterpret_cast<void*>(0x2001));
    CHECK(state_of("hook:A") == ResourceState::kLive);  // gate 开
    ResourceRuntime runtime_a{};
    CHECK(runtime_of("hook:A", &runtime_a));
    CHECK(runtime_a.resolved_entry == 0x1000);
    CHECK(runtime_a.backup == reinterpret_cast<void*>(0x2001));
    CHECK(runtime_a.in_flight == 0);
    CHECK(!runtime_a.isolated);

    TxStep second{};
    CHECK(!tx_step(tx, &second));
    CHECK(std::strcmp(second.resource_id, "hook:B") == 0);
    CHECK(second.ok == 0);
    CHECK(second.finished);
    CHECK(second.backup == reinterpret_cast<void*>(0x2002));
    CHECK(state_of("hook:B") == ResourceState::kDisabled);  // gate 关

    CHECK(g_hook_calls == 2);
    CHECK(g_hook_targets[1] == reinterpret_cast<void*>(0x1000));
    CHECK(g_hook_replacements[1] == &g_handler_a);
    CHECK(g_hook_replacements[2] == &g_handler_b);
    CHECK(g_unhook_calls == 0);

    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kSucceeded);
    CHECK(status.step == 2);
    CHECK(status.frames == 2);
    CHECK(status.failed_resource_id == nullptr);
    CHECK(status.error == nullptr);
}

void test_install_second_fails_rollback() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    CHECK(register_claim(make_claim("hook:B", "plugin_a")));
    g_hook_fail_at = 2;  // 第二步安装失败

    const uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
    TxStep step{};
    CHECK(tx_step(tx, &step));  // A 成功
    CHECK(state_of("hook:A") == ResourceState::kLive);

    TxStep failed{};
    CHECK(!tx_step(tx, &failed));
    CHECK(failed.finished);
    CHECK(failed.ok != 0);
    CHECK(std::strcmp(failed.resource_id, "hook:B") == 0);

    // 已安装子集逆序回滚：A 回到 free 且运行期清除。
    CHECK(state_of("hook:A") == ResourceState::kFree);
    ResourceRuntime runtime_a{};
    CHECK(!runtime_of("hook:A", &runtime_a));
    CHECK(g_unhook_calls == 1);
    CHECK(g_last_unhook_arg == reinterpret_cast<void*>(0x1000));
    CHECK(state_of("hook:B") == ResourceState::kFree);
    CHECK(!isolated_get("hook:A"));
    CHECK(!isolated_get("hook:B"));

    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(status.step == 1);
    CHECK(status.failed_resource_id != nullptr);
    CHECK(std::strcmp(status.failed_resource_id, "hook:B") == 0);
    CHECK(status.error != nullptr);
    CHECK(std::strcmp(status.error, "native hook failed") == 0);
}

void test_rollback_unhook_failure_isolates() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    CHECK(register_claim(make_claim("hook:B", "plugin_a")));
    g_hook_fail_at = 2;
    g_unhook_fail_at = 1;  // 回滚 A 时物理释放失败

    const uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
    TxStep step{};
    CHECK(tx_step(tx, &step));
    TxStep failed{};
    CHECK(!tx_step(tx, &failed));
    CHECK(failed.finished);

    CHECK(state_of("hook:A") == ResourceState::kTombstone);
    CHECK(isolated_get("hook:A"));

    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(std::strcmp(status.failed_resource_id, "hook:B") == 0);
    CHECK(std::strcmp(status.error, "native hook failed") == 0);
}

void test_resumable_false_skips_unhook() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a", "gateOn", false)));
    CHECK(register_claim(make_claim("hook:B", "plugin_a")));
    g_hook_fail_at = 2;

    const uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
    TxStep step{};
    CHECK(tx_step(tx, &step));
    CHECK(state_of("hook:A") == ResourceState::kLive);
    TxStep failed{};
    CHECK(!tx_step(tx, &failed));

    // resumable=false：回滚不调用 unhook，直接保留占用并隔离。
    CHECK(g_unhook_calls == 0);
    CHECK(state_of("hook:A") == ResourceState::kTombstone);
    CHECK(isolated_get("hook:A"));
    CHECK(std::strcmp(resource_state_name(state_of("hook:A")), "tombstone") == 0);

    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(std::strcmp(status.failed_resource_id, "hook:B") == 0);
    CHECK(std::strcmp(status.error, "native hook failed") == 0);
}

void test_restore_waits_for_in_flight() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    const uint64_t install_tx = install_one("plugin_a");
    CHECK(install_tx != 0);
    CHECK(state_of("hook:A") == ResourceState::kLive);

    const uint64_t tx = tx_begin("plugin_a", TxKind::kRelease);
    TxStatus status{};
    {
        InFlightGuard wrapper_guard("hook:A");
        CHECK(wrapper_guard.entered());
        CHECK(in_flight_count("hook:A") == 1);

        CHECK(tx != 0);
        TxStep waiting{};
        CHECK(tx_step(tx, &waiting));  // 在途未归零：只耗帧
        CHECK(waiting.resource_id == nullptr);
        CHECK(waiting.ok == 0);
        CHECK(!waiting.finished);
        CHECK(state_of("hook:A") == ResourceState::kLive);

        CHECK(tx_query(tx, &status));
        CHECK(status.step == 0);
        CHECK(status.frames == 1);
        CHECK(status.total == 1);
        CHECK(status.kind == TxKind::kRelease);
    }
    CHECK(in_flight_count("hook:A") == 0);
    TxStep done{};
    CHECK(!tx_step(tx, &done));
    CHECK(done.resource_id != nullptr);
    CHECK(std::strcmp(done.resource_id, "hook:A") == 0);
    CHECK(done.finished);
    CHECK(state_of("hook:A") == ResourceState::kFree);
    CHECK(g_unhook_calls == 1);
    CHECK(g_last_unhook_arg == reinterpret_cast<void*>(0x1000));

    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kSucceeded);
    CHECK(status.step == 1);
    CHECK(status.frames == 2);
}

void test_restore_in_flight_timeout() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a", "gateOn", true, 3)));
    CHECK(install_one("plugin_a") != 0);
    CHECK(in_flight_enter("hook:A"));

    const uint64_t tx = tx_begin("plugin_a", TxKind::kRelease);
    CHECK(tx != 0);
    // 预算 3：前 3 个等待帧允许，第 4 个等待帧超预算。
    for (int i = 0; i < 3; ++i) {
        TxStep waiting{};
        CHECK(tx_step(tx, &waiting));
        CHECK(waiting.resource_id == nullptr);
        CHECK(!waiting.finished);
    }
    TxStep timeout{};
    CHECK(!tx_step(tx, &timeout));
    CHECK(timeout.finished);
    CHECK(timeout.ok != 0);
    CHECK(std::strcmp(timeout.resource_id, "hook:A") == 0);

    CHECK(state_of("hook:A") == ResourceState::kTombstone);
    CHECK(isolated_get("hook:A"));
    CHECK(g_unhook_calls == 0);

    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(status.frames == 4);
    CHECK(status.budget == 3);
    CHECK(status.failed_resource_id != nullptr);
    CHECK(std::strcmp(status.failed_resource_id, "hook:A") == 0);
    CHECK(status.error != nullptr);
    CHECK(std::strcmp(status.error, "in_flight_timeout") == 0);
}

void test_isolated_blocks_begin() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    CHECK(isolated_set("hook:A", true));
    CHECK(tx_begin("plugin_a", TxKind::kInstall) == 0);
    CHECK(isolated_set("hook:A", false));
    CHECK(tx_begin("plugin_a", TxKind::kInstall) != 0);
}

void test_same_plugin_concurrent_rejected() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    CHECK(register_claim(make_claim("hook:B", "plugin_a")));

    const uint64_t first = tx_begin("plugin_a", TxKind::kInstall);
    CHECK(first != 0);
    CHECK(tx_begin("plugin_a", TxKind::kInstall) == 0);  // 未终态事务存在
    CHECK(tx_begin("plugin_b", TxKind::kInstall) == 0);  // 该插件无资源

    TxStep step{};
    CHECK(tx_step(first, &step));   // A 完成，仍需继续
    CHECK(!tx_step(first, &step));  // B 完成，事务终态
    CHECK(step.finished);
    CHECK(tx_begin("plugin_a", TxKind::kInstall) != 0);  // 终态后允许
}

void test_install_live_is_idempotent_skip() {
    reset_all();
    set_deps();
    const ResourceClaim claim = make_claim("hook:A", "plugin_a");
    CHECK(register_claim(claim));
    const InstallOutcome installed = install_claim_entry_hook(claim, 0x1000, fake_hook, fake_unhook, true);
    CHECK(installed.ok == 0);
    CHECK(state_of("hook:A") == ResourceState::kLive);
    CHECK(g_hook_calls == 1);

    const uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished);
    CHECK(step.ok == 0);
    CHECK(step.backup == nullptr);  // 跳过不产生新 backup
    CHECK(std::strcmp(step.resource_id, "hook:A") == 0);
    CHECK(g_hook_calls == 1);  // 无第二次机制调用
    CHECK(state_of("hook:A") == ResourceState::kLive);

    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kSucceeded);
    CHECK(status.step == 1);
}

void test_query_terminal_and_reset() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    const uint64_t tx = install_one("plugin_a");
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kSucceeded);

    tx_reset_for_test();
    CHECK(!tx_query(tx, &status));  // 已淘汰/归零
    // deps 一并归零：重新开始事务被拒。
    CHECK(tx_begin("plugin_a", TxKind::kInstall) == 0);

    // 再次装配后可正常运行，且 id 重新从 1 开始单调。
    set_deps();
    const uint64_t again = tx_begin("plugin_a", TxKind::kInstall);
    CHECK(again == 1);
    CHECK(tx_query(again, &status));
    CHECK(status.state == TxState::kRunning);
}

void test_install_anchor_failures() {
    reset_all();
    set_deps();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));

    // 非 free（tombstone）→ 本步失败，不回滚（无已安装子集）。
    CHECK(set_state("hook:A", ResourceState::kTombstone));
    uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished);
    CHECK(step.ok != 0);
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(status.error != nullptr);
    CHECK(std::strcmp(status.error, "resource not free") == 0);
    CHECK(status.failed_resource_id != nullptr);
    CHECK(std::strcmp(status.failed_resource_id, "hook:A") == 0);
    CHECK(g_hook_calls == 0);

    // 解析失败 → "symbol not resolved"；失败后未隔离，可重试。
    CHECK(set_state("hook:A", ResourceState::kFree));
    g_resolve_value = 0;
    tx = tx_begin("plugin_a", TxKind::kInstall);
    CHECK(tx != 0);
    CHECK(!tx_step(tx, &step));
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(std::strcmp(status.error, "symbol not resolved") == 0);
    CHECK(g_hook_calls == 0);
}

void test_release_anchors() {
    reset_all();
    set_deps();
    const ResourceClaim claim = make_claim("hook:A", "plugin_a");

    // 未登记。
    ReleaseOutcome outcome = release_claim_entry_hook(claim, 0x1000, fake_unhook);
    CHECK(outcome.ok != 0);
    CHECK(std::strcmp(outcome.error, "claim not registered") == 0);

    CHECK(register_claim(claim));
    // mechanism 不匹配。
    ResourceClaim mismatch = claim;
    mismatch.mechanism = ResourceMechanism::kCallPatch;
    outcome = release_claim_entry_hook(mismatch, 0x1000, fake_unhook);
    CHECK(std::strcmp(outcome.error, "mechanism mismatch") == 0);
    // 未就绪：入口为 0 或 unhook 为空。
    outcome = release_claim_entry_hook(claim, 0, fake_unhook);
    CHECK(std::strcmp(outcome.error, "not ready") == 0);
    outcome = release_claim_entry_hook(claim, 0x1000, nullptr);
    CHECK(std::strcmp(outcome.error, "not ready") == 0);
    CHECK(g_unhook_calls == 0);

    // unhook 失败：状态不变，运行期记录保留。
    CHECK(install_claim_entry_hook(claim, 0x1000, fake_hook, fake_unhook, true).ok == 0);
    CHECK(state_of("hook:A") == ResourceState::kLive);
    CHECK(runtime_set("hook:A", 0x1000, reinterpret_cast<void*>(0x2000)));
    g_unhook_fail_at = 1;
    outcome = release_claim_entry_hook(claim, 0x1000, fake_unhook);
    CHECK(outcome.ok != 0);
    CHECK(std::strcmp(outcome.error, "native unhook failed") == 0);
    CHECK(state_of("hook:A") == ResourceState::kLive);
    ResourceRuntime runtime{};
    CHECK(runtime_of("hook:A", &runtime));

    // 成功：状态 kFree 且运行期清除。
    g_unhook_fail_at = -1;
    outcome = release_claim_entry_hook(claim, 0x1000, fake_unhook);
    CHECK(outcome.ok == 0);
    CHECK(outcome.error == nullptr);
    CHECK(state_of("hook:A") == ResourceState::kFree);
    CHECK(!runtime_of("hook:A", &runtime));
}

void test_terminal_retention_evicts_oldest() {
    reset_all();
    set_deps();
    const ResourceClaim claim = make_claim("hook:A", "plugin_a");
    CHECK(register_claim(claim));
    // 直接装到 kLive：后续安装事务都走幂等跳过路径，每步即终态。
    CHECK(install_claim_entry_hook(claim, 0x1000, fake_hook, fake_unhook, true).ok == 0);

    uint64_t first_id = 0;
    uint64_t last_id = 0;
    for (int i = 0; i < 17; ++i) {
        const uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
        CHECK(tx != 0);
        if (i == 0) {
            first_id = tx;
        }
        last_id = tx;
        TxStep step{};
        CHECK(!tx_step(tx, &step));
        CHECK(step.finished);
    }
    TxStatus status{};
    CHECK(!tx_query(first_id, &status));  // 超出 16 条，最旧被淘汰
    CHECK(tx_query(last_id, &status));
    CHECK(status.state == TxState::kSucceeded);
}

// kCallPatch 资源走宿主注入的 install_other/release_other；入口 hook 路径不受影响。
void test_call_patch_mechanism_transaction() {
    reset_all();
    set_deps();
    ResourceClaim claim = make_claim("patch:A", "patch_plugin", "gateOn");
    claim.mechanism = ResourceMechanism::kCallPatch;
    claim.handler = &g_handler_a;
    CHECK(register_claim(claim));

    // gate 开：调 install_other + 状态 kLive；调用点 patch 无 backup。
    uint64_t tx = tx_begin("patch_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished);
    CHECK(step.resource_id != nullptr);
    CHECK(std::strcmp(step.resource_id, "patch:A") == 0);
    CHECK(step.ok == 0);
    CHECK(step.backup == nullptr);
    CHECK(g_other_install_calls == 1);
    CHECK(g_other_last_claim != nullptr);
    CHECK(std::strcmp(g_other_last_claim->id, "patch:A") == 0);
    CHECK(state_of("patch:A") == ResourceState::kLive);
    CHECK(g_hook_calls == 0);  // 不得误走入口 hook 原语

    // 还原：调 release_other，成功后 kFree。
    tx = tx_begin("patch_plugin", TxKind::kRelease);
    CHECK(tx != 0);
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished);
    CHECK(g_other_release_calls == 1);
    CHECK(state_of("patch:A") == ResourceState::kFree);

    // gate 关：call patch 不支持空转，不安装、状态保持 free。
    reset_all();
    set_deps();
    ResourceClaim off = make_claim("patch:B", "patch_plugin", "gateOff");
    off.mechanism = ResourceMechanism::kCallPatch;
    off.handler = &g_handler_a;
    CHECK(register_claim(off));
    tx = tx_begin("patch_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished);
    CHECK(g_other_install_calls == 0);
    CHECK(state_of("patch:B") == ResourceState::kFree);

    // install_other 失败：事务失败并记录原因，资源保持 free。
    reset_all();
    set_deps();
    g_other_install_rc = 1;
    ResourceClaim fail = make_claim("patch:C", "patch_plugin", "gateOn");
    fail.mechanism = ResourceMechanism::kCallPatch;
    fail.handler = &g_handler_a;
    CHECK(register_claim(fail));
    tx = tx_begin("patch_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished);
    CHECK(step.ok != 0);
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(status.error != nullptr);
    CHECK(std::strcmp(status.error, "primitive rejected") == 0);
    CHECK(status.failed_resource_id != nullptr);
    CHECK(std::strcmp(status.failed_resource_id, "patch:C") == 0);
    CHECK(state_of("patch:C") == ResourceState::kFree);
}

// 非入口机制安装成功必须把机制层确证的原函数入口原样交还（kPtrSlot 的原值）；
// 失败/软失败/门禁关/回滚不得泄漏 handler，也不得复用上一步的 backup——
// feature-mode 以 out.backup != null 作为「本步真实安装」判据并据此调 on_resource_ready。
void test_non_entry_backup_handover_transaction() {
    void* const sentinel = reinterpret_cast<void*>(0x5000);

    // 聚合初始化的默认 backup 必须为 null：kCallPatch/kOwnership 靠该默认值。
    CHECK((MechanismOutcome{0, nullptr}.backup) == nullptr);
    CHECK((MechanismOutcome{1, "x"}.backup) == nullptr);

    reset_all();
    set_deps();
    g_other_install_backup = sentinel;

    ResourceClaim slot_a = make_claim("slot:A", "slot_plugin", "gateOn");
    slot_a.mechanism = ResourceMechanism::kPtrSlot;
    ResourceClaim slot_b = make_claim("slot:B", "slot_plugin", "gateOn");
    slot_b.mechanism = ResourceMechanism::kPtrSlot;
    CHECK(register_claim(slot_a));
    CHECK(register_claim(slot_b));

    // 成功步：backup 原样交还（不是 handler，也不是空）。
    uint64_t tx = tx_begin("slot_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(tx_step(tx, &step));  // A 成功、事务仍在途
    CHECK(step.ok == 0);
    CHECK(std::strcmp(step.resource_id, "slot:A") == 0);
    CHECK(step.backup == sentinel);
    CHECK(step.backup != &g_handler_a);
    CHECK(state_of("slot:A") == ResourceState::kLive);

    // 失败步：不带任何 backup（既不泄漏 handler，也不带上一步的 sentinel），并逆序回滚已装子集。
    g_other_install_rc = 2;
    CHECK(!tx_step(tx, &step));  // B 失败 → 终态
    CHECK(step.finished);
    CHECK(step.ok != 0);  // FailClosed 走通用失败码
    CHECK(step.backup == nullptr);
    CHECK(step.backup != sentinel && step.backup != &g_handler_a);
    CHECK(state_of("slot:A") == ResourceState::kFree);
    CHECK(state_of("slot:B") == ResourceState::kFree);
    CHECK(g_other_release_calls == 1);  // 回滚释放已装子集

    // 软失败：机制带 sentinel 但本步失败 → 仍为 null（软失败不冒充成功交还）。
    reset_all();
    set_deps();
    g_other_install_backup = sentinel;
    ResourceClaim soft = make_claim("slot:soft", "slot_plugin", "gateOn");
    soft.mechanism = ResourceMechanism::kPtrSlot;
    soft.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(soft));
    g_other_install_rc = 7;
    tx = tx_begin("slot_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    CHECK(!tx_step(tx, &step));  // 唯一资源软失败即终态成功
    CHECK(step.ok == 7);
    CHECK(step.backup == nullptr);
    CHECK(step.backup != sentinel);
    CHECK(state_of("slot:soft") == ResourceState::kFree);

    // 门禁关：非入口机制不安装（无空转），backup 保持 null。
    reset_all();
    set_deps();
    g_other_install_backup = sentinel;
    ResourceClaim off = make_claim("slot:off", "slot_plugin", "gateOff");
    off.mechanism = ResourceMechanism::kPtrSlot;
    CHECK(register_claim(off));
    tx = tx_begin("slot_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    CHECK(!tx_step(tx, &step));
    CHECK(step.ok == 0 && step.backup == nullptr);
    CHECK(g_other_install_calls == 0);
    CHECK(state_of("slot:off") == ResourceState::kFree);

    // 中止安装：已成功步交还过 sentinel 之后 tx_abort_install 回滚；终态查询不带 backup。
    reset_all();
    set_deps();
    g_other_install_backup = sentinel;
    ResourceClaim ab = make_claim("slot:abort", "slot_plugin", "gateOn");
    ab.mechanism = ResourceMechanism::kPtrSlot;
    ResourceClaim ab2 = make_claim("slot:abort2", "slot_plugin", "gateOn");
    ab2.mechanism = ResourceMechanism::kPtrSlot;
    CHECK(register_claim(ab));
    CHECK(register_claim(ab2));
    tx = tx_begin("slot_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    CHECK(tx_step(tx, &step));
    CHECK(step.backup == sentinel);
    CHECK(tx_abort_install(tx, "slot:abort", "restricted callback failed"));
    CHECK(state_of("slot:abort") == ResourceState::kFree);
    CHECK(state_of("slot:abort2") == ResourceState::kFree);
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    // 终态后再次步进不产生新步骤，也不复活 backup。
    step = TxStep{};
    CHECK(!tx_step(tx, &step));
    CHECK(step.backup == nullptr);
}

// kOwnership 资源经事务引擎：acquire 按声明序、release 逆序；不触入口 hook 原语。
void test_ownership_mechanism_transaction() {
    reset_all();
    TxDeps deps{};
    deps.resolve_entry = fake_resolve;
    deps.hook_fn = fake_hook;
    deps.unhook_fn = fake_unhook;
    deps.gate_on = fake_gate;
    deps.install_other = ownership_install_other;
    deps.release_other = ownership_release_other;
    tx_set_deps(deps);

    const char* ids[] = {"own:A", "own:B", "own:C"};
    for (const char* id : ids) {
        ResourceClaim c = make_claim(id, "own_plugin", "gateOn");
        c.mechanism = ResourceMechanism::kOwnership;
        c.symbol = "ownership:test";
        c.handler = const_cast<qol::contracts::OwnershipOps*>(&g_probe_ownership_ops);
        CHECK(register_claim(c));
    }

    uint64_t tx = tx_begin("own_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(tx_step(tx, &step));   // A
    CHECK(tx_step(tx, &step));   // B
    CHECK(!tx_step(tx, &step));  // C（终态）
    CHECK(step.finished);
    CHECK(g_own_acquire_calls == 3);
    CHECK(std::strcmp(g_own_acquire_order[0], "own:A") == 0);
    CHECK(std::strcmp(g_own_acquire_order[1], "own:B") == 0);
    CHECK(std::strcmp(g_own_acquire_order[2], "own:C") == 0);
    CHECK(g_hook_calls == 0);  // 不得误走入口 hook 原语
    CHECK(state_of("own:A") == ResourceState::kLive);
    CHECK(state_of("own:B") == ResourceState::kLive);
    CHECK(state_of("own:C") == ResourceState::kLive);

    // 还原：事务逆序 release（C → B → A）。
    tx = tx_begin("own_plugin", TxKind::kRelease);
    CHECK(tx != 0);
    CHECK(tx_step(tx, &step));
    CHECK(tx_step(tx, &step));
    CHECK(!tx_step(tx, &step));
    CHECK(g_own_release_calls == 3);
    CHECK(std::strcmp(g_own_release_order[0], "own:C") == 0);
    CHECK(std::strcmp(g_own_release_order[1], "own:B") == 0);
    CHECK(std::strcmp(g_own_release_order[2], "own:A") == 0);
    CHECK(state_of("own:A") == ResourceState::kFree);
    CHECK(state_of("own:B") == ResourceState::kFree);
    CHECK(state_of("own:C") == ResourceState::kFree);
}

void test_required_alias_group_preflight_prevents_first_hook_write() {
    reset_all();
    set_deps();
    g_alias_symbol_a = "elf-export:GroupAliasA";
    g_alias_symbol_b = "elf-export:GroupAliasB";
    g_alias_target = 0x7400;
    ResourceClaim first = make_claim("hook:alias:group:a", "alias_group");
    ResourceClaim second = make_claim("hook:alias:group:b", "alias_group");
    first.symbol = g_alias_symbol_a;
    second.symbol = g_alias_symbol_b;
    CHECK(register_claim(first));
    CHECK(register_claim(second));

    const uint64_t tx = tx_begin("alias_group", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished && step.ok != 0);
    CHECK(g_hook_calls == 0);
    CHECK(state_of(first.id) == ResourceState::kFree);
    CHECK(state_of(second.id) == ResourceState::kFree);
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(status.failed_resource_id != nullptr &&
          std::strcmp(status.failed_resource_id, second.id) == 0);
    CHECK(status.error != nullptr && std::strcmp(status.error, "duplicate entry target") == 0);
}

void test_alias_can_be_reused_after_successful_physical_release() {
    reset_all();
    set_deps();
    g_alias_symbol_a = "elf-export:ReusableAliasA";
    g_alias_symbol_b = "elf-export:ReusableAliasB";
    g_alias_target = 0x7500;
    ResourceClaim first = make_claim("hook:alias:reusable:a", "alias_first");
    ResourceClaim second = make_claim("hook:alias:reusable:b", "alias_second");
    first.symbol = g_alias_symbol_a;
    second.symbol = g_alias_symbol_b;
    CHECK(register_claim(first));
    CHECK(register_claim(second));

    const uint64_t first_install = install_one("alias_first");
    CHECK(first_install != 0);
    CHECK(g_hook_calls == 1);
    const uint64_t blocked_install = tx_begin("alias_second", TxKind::kInstall);
    CHECK(blocked_install != 0);
    TxStep step{};
    CHECK(!tx_step(blocked_install, &step));
    CHECK(g_hook_calls == 1);
    CHECK(state_of(first.id) == ResourceState::kLive);
    CHECK(state_of(second.id) == ResourceState::kFree);

    const uint64_t release = tx_begin("alias_first", TxKind::kRelease);
    CHECK(release != 0);
    CHECK(!tx_step(release, &step));
    CHECK(step.finished && step.ok == 0);
    CHECK(state_of(first.id) == ResourceState::kFree);
    ResourceRuntime runtime{};
    CHECK(!runtime_of(first.id, &runtime));

    const uint64_t second_install = tx_begin("alias_second", TxKind::kInstall);
    CHECK(second_install != 0);
    CHECK(!tx_step(second_install, &step));
    CHECK(step.finished && step.ok == 0);
    CHECK(g_hook_calls == 2);
    CHECK(state_of(second.id) == ResourceState::kLive);
}

void test_isolated_tombstone_keeps_alias_target_reserved() {
    reset_all();
    set_deps();
    g_alias_symbol_a = "elf-export:TombstoneAliasA";
    g_alias_symbol_b = "elf-export:TombstoneAliasB";
    g_alias_target = 0x7600;
    ResourceClaim first = make_claim("hook:alias:tombstone:a", "alias_tombstone");
    ResourceClaim second = make_claim("hook:alias:tombstone:b", "alias_after_tombstone");
    first.symbol = g_alias_symbol_a;
    second.symbol = g_alias_symbol_b;
    CHECK(register_claim(first));
    CHECK(register_claim(second));
    CHECK(install_one("alias_tombstone") != 0);

    g_unhook_fail_at = 1;
    const uint64_t release = tx_begin("alias_tombstone", TxKind::kRelease);
    CHECK(release != 0);
    TxStep step{};
    CHECK(!tx_step(release, &step));
    CHECK(step.finished && step.ok != 0);
    CHECK(state_of(first.id) == ResourceState::kTombstone);
    CHECK(isolated_get(first.id));
    const uint64_t retry = tx_begin("alias_after_tombstone", TxKind::kInstall);
    CHECK(retry != 0);
    CHECK(!tx_step(retry, &step));
    CHECK(step.finished && step.ok != 0);
    CHECK(g_hook_calls == 1);
    CHECK(state_of(second.id) == ResourceState::kFree);
}

// FailSoft 入口 hook 安装失败：保持该资源 Free、不写入 runtime/backup，报告本步失败原因
// 并继续后续资源；已成功的资源不回滚，事务最终 Succeeded。
void test_fail_soft_middle_failure_continues() {
    reset_all();
    set_deps();
    ResourceClaim a = make_claim("hook:soft:A", "soft_plugin");
    ResourceClaim b = make_claim("hook:soft:B", "soft_plugin");
    ResourceClaim c = make_claim("hook:soft:C", "soft_plugin");
    b.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(a));
    CHECK(register_claim(b));
    CHECK(register_claim(c));

    g_hook_fail_at = 2;
    const uint64_t tx = tx_begin("soft_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(tx_step(tx, &step));  // A 成功
    CHECK(step.ok == 0 && step.error == nullptr);
    CHECK(tx_step(tx, &step));  // B 软失败
    CHECK(step.resource_id != nullptr && std::strcmp(step.resource_id, "hook:soft:B") == 0);
    CHECK(step.ok != 0);
    CHECK(step.error != nullptr && std::strcmp(step.error, "native hook failed") == 0);
    CHECK(step.backup == nullptr);
    CHECK(!step.finished);
    CHECK(state_of("hook:soft:B") == ResourceState::kFree);
    ResourceRuntime runtime{};
    CHECK(!runtime_of("hook:soft:B", &runtime));
    CHECK(!tx_step(tx, &step));  // C 成功（终态）
    CHECK(step.finished && step.ok == 0);

    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kSucceeded);
    CHECK(g_unhook_calls == 0);  // 已装成功资源不回滚
    CHECK(state_of("hook:soft:A") == ResourceState::kLive);
    CHECK(state_of("hook:soft:C") == ResourceState::kLive);
}

// 软失败发生在最后一条：事务仍 Succeeded，末步保留失败原因，资源保持 Free。
void test_fail_soft_last_failure_still_succeeds() {
    reset_all();
    set_deps();
    ResourceClaim a = make_claim("hook:softlast:A", "soft_last");
    ResourceClaim b = make_claim("hook:softlast:B", "soft_last");
    b.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(a));
    CHECK(register_claim(b));

    g_hook_fail_at = 2;
    const uint64_t tx = tx_begin("soft_last", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(tx_step(tx, &step));
    CHECK(!tx_step(tx, &step));  // 末条软失败即终态
    CHECK(step.finished && step.ok != 0);
    CHECK(step.error != nullptr && std::strcmp(step.error, "native hook failed") == 0);
    CHECK(state_of("hook:softlast:B") == ResourceState::kFree);

    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kSucceeded);
}

// 软失败后仍遇 FailClosed 硬失败：事务按逆序回滚真实安装子集并 Failed；
// 软失败资源保持 Free，不被伪造成已安装回滚。
void test_fail_soft_then_hard_failure_rolls_back() {
    reset_all();
    set_deps();
    ResourceClaim soft = make_claim("hook:softhard:soft", "soft_hard");
    ResourceClaim hard = make_claim("hook:softhard:hard", "soft_hard");
    soft.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(soft));
    CHECK(register_claim(hard));

    g_hook_fail_at = 1;  // 第一条软失败
    const uint64_t tx = tx_begin("soft_hard", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(tx_step(tx, &step));
    CHECK(step.ok != 0 && !step.finished);
    CHECK(state_of("hook:softhard:soft") == ResourceState::kFree);

    // 第二条改为硬失败。
    g_hook_fail_at = 2;
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished && step.ok != 0 && step.resource_id != nullptr &&
          std::strcmp(step.resource_id, "hook:softhard:hard") == 0);
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(g_unhook_calls == 0);
    CHECK(state_of("hook:softhard:soft") == ResourceState::kFree);
    CHECK(state_of("hook:softhard:hard") == ResourceState::kFree);
}

// 非入口机制（kCallPatch）软失败：保持 Free 并继续，事务 Succeeded。
void test_fail_soft_non_entry_mechanism() {
    reset_all();
    set_deps();
    ResourceClaim claim = make_claim("patch:soft:A", "patch_soft", "gateOn");
    claim.mechanism = ResourceMechanism::kCallPatch;
    claim.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(claim));

    g_other_install_rc = 1;
    const uint64_t tx = tx_begin("patch_soft", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished && step.ok != 0);
    CHECK(step.error != nullptr && std::strcmp(step.error, "primitive rejected") == 0);
    CHECK(state_of("patch:soft:A") == ResourceState::kFree);

    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kSucceeded);
}

// 同事务内已被安全撤回（Free + runtime 已清）的条目不得二次释放：否则会把已归还资源
// 错误标记 tombstone/isolated。Free 但仍保留 runtime 的不安全状态继续走释放路径。
void test_rollback_skips_safely_revoked_entry() {
    reset_all();
    set_deps();
    ResourceClaim a = make_claim("hook:rb:A", "rb_plugin");
    ResourceClaim b = make_claim("hook:rb:B", "rb_plugin");
    CHECK(register_claim(a));
    CHECK(register_claim(b));

    const uint64_t tx = tx_begin("rb_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(tx_step(tx, &step));  // A 安装成功
    CHECK(state_of("hook:rb:A") == ResourceState::kLive);

    // 模拟宿主软 ready 失败后的安全撤回。
    set_state("hook:rb:A", ResourceState::kFree);
    runtime_clear("hook:rb:A");
    const int unhooks_before = g_unhook_calls;

    g_hook_fail_at = 2;  // B 硬失败
    CHECK(!tx_step(tx, &step));
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(g_unhook_calls == unhooks_before);  // 不对已撤回的 A 二次 unhook
    CHECK(state_of("hook:rb:A") == ResourceState::kFree);
    CHECK(!isolated_get("hook:rb:A"));
    CHECK(state_of("hook:rb:B") == ResourceState::kFree);
}

// Free 但 runtime 记录仍在（不安全状态）：回滚仍执行释放，不跳过、不谎报已归还。
void test_rollback_releases_free_entry_with_runtime() {
    reset_all();
    set_deps();
    ResourceClaim a = make_claim("hook:rb2:A", "rb2_plugin");
    ResourceClaim b = make_claim("hook:rb2:B", "rb2_plugin");
    CHECK(register_claim(a));
    CHECK(register_claim(b));

    const uint64_t tx = tx_begin("rb2_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(tx_step(tx, &step));

    // Free 但保留 runtime（未清）：回滚必须真实释放。
    set_state("hook:rb2:A", ResourceState::kFree);
    const int unhooks_before = g_unhook_calls;
    g_hook_fail_at = 2;
    CHECK(!tx_step(tx, &step));
    CHECK(g_unhook_calls == unhooks_before + 1);
    CHECK(state_of("hook:rb2:A") == ResourceState::kFree);
}

// FailSoft 软失败步必须保留原始 mechanism 失败码（host 据此区分 NotReady 与永久错误）；
// 事务仍继续并成功终态，资源保持 Free。
void test_fail_soft_preserves_mechanism_code() {
    reset_all();
    set_deps();
    ResourceClaim claim = make_claim("patch:soft:code", "patch_soft_code", "gateOn");
    claim.mechanism = ResourceMechanism::kCallPatch;
    claim.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(claim));

    g_other_install_rc = 3;  // kQolErrNotReady
    const uint64_t tx = tx_begin("patch_soft_code", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished);
    CHECK(step.ok == 3);  // 原码保留，不再统一写成 1
    CHECK(step.error != nullptr && std::strcmp(step.error, "primitive rejected") == 0);
    CHECK(state_of("patch:soft:code") == ResourceState::kFree);
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kSucceeded);
}

// FailSoft 入口 hook 软失败保持既有 1（无 mechanism 码可保留），语义不变。
void test_fail_soft_entry_hook_keeps_generic_code() {
    reset_all();
    set_deps();
    ResourceClaim claim = make_claim("hook:soft:code", "hook_soft_code");
    claim.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(claim));

    g_hook_fail_at = 1;
    const uint64_t tx = tx_begin("hook_soft_code", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(!tx_step(tx, &step));
    CHECK(step.finished && step.ok == 1);
    CHECK(step.error != nullptr && std::strcmp(step.error, "native hook failed") == 0);
    CHECK(state_of("hook:soft:code") == ResourceState::kFree);
}

// tx_abort_install：仅 Running 安装事务可中止；中止即回滚并终态化，禁止后续步继续安装。
void test_tx_abort_install_stops_running_install() {
    reset_all();
    set_deps();
    ResourceClaim a = make_claim("hook:ab:A", "ab_plugin");
    ResourceClaim b = make_claim("hook:ab:B", "ab_plugin");
    CHECK(register_claim(a));
    CHECK(register_claim(b));

    const uint64_t tx = tx_begin("ab_plugin", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    CHECK(tx_step(tx, &step));  // A 安装成功，事务仍在途
    CHECK(state_of("hook:ab:A") == ResourceState::kLive);

    CHECK(tx_abort_install(tx, "hook:ab:B", "resource_ready_failed"));
    CHECK(state_of("hook:ab:A") == ResourceState::kFree);  // 已装子集逆序回滚
    CHECK(g_unhook_calls == 1);
    TxStatus status{};
    CHECK(tx_query(tx, &status));
    CHECK(status.state == TxState::kFailed);
    CHECK(status.error != nullptr &&
          std::strcmp(status.error, "resource_ready_failed") == 0);
    CHECK(status.failed_resource_id != nullptr &&
          std::strcmp(status.failed_resource_id, "hook:ab:B") == 0);

    // 已终态：再次中止返回 false，且不继续安装剩余资源。
    CHECK(!tx_abort_install(tx, "hook:ab:B", "again"));
    CHECK(!tx_step(tx, &step));
    CHECK(g_hook_calls == 1);  // B 从未安装
    CHECK(state_of("hook:ab:B") == ResourceState::kFree);
}

// 非安装事务/未知 id 不可中止。
void test_tx_abort_install_rejects_non_install() {
    reset_all();
    set_deps();
    ResourceClaim a = make_claim("hook:abn:A", "abn_plugin");
    CHECK(register_claim(a));
    CHECK(install_one("abn_plugin") != 0);

    const uint64_t release = tx_begin("abn_plugin", TxKind::kRelease);
    CHECK(release != 0);
    CHECK(!tx_abort_install(release, "hook:abn:A", "x"));  // 还原事务不可中止
    CHECK(!tx_abort_install(0, "hook:abn:A", "x"));
    CHECK(!tx_abort_install(999999, "hook:abn:A", "x"));
    TxStep step{};
    CHECK(!tx_step(release, &step));  // 还原仍正常完成
    CHECK(state_of("hook:abn:A") == ResourceState::kFree);
}

// 真实 EntryHook 成功但 backup 为空：撤销成功 → 按资源失败策略处理（fail-soft 保持 free）。
void test_install_null_backup_unhook_success_soft_fail() {
    reset_all();
    set_deps();
    ResourceClaim a = make_claim("hook:A", "plugin_a");
    a.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(a));
    g_hook_no_backup_at = 1;

    const uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    const bool more = tx_step(tx, &step);
    CHECK(!more);
    CHECK(step.finished);
    CHECK(step.ok != 0);
    CHECK(std::strcmp(step.error, "resource_ready_failed") == 0);
    CHECK(g_unhook_calls == 1);
    CHECK(state_of("hook:A") == ResourceState::kFree);
    CHECK(!isolated_get("hook:A"));
    ResourceRuntime rt{};
    CHECK(!runtime_of("hook:A", &rt));
}

// 真实 EntryHook 成功但 backup 为空且撤销失败：硬失败、tombstone + isolated；即使 fail-soft
// 也不得清回 free 或留下可重试状态。
void test_install_null_backup_unhook_fail_hard_even_fail_soft() {
    reset_all();
    set_deps();
    ResourceClaim a = make_claim("hook:A", "plugin_a");
    a.failure = ResourceFailure::kFailSoft;
    CHECK(register_claim(a));
    g_hook_no_backup_at = 1;
    g_unhook_fail_at = 1;

    const uint64_t tx = tx_begin("plugin_a", TxKind::kInstall);
    CHECK(tx != 0);
    TxStep step{};
    const bool more = tx_step(tx, &step);
    CHECK(!more);
    CHECK(step.finished);
    CHECK(step.ok != 0);
    CHECK(std::strcmp(step.error, "native unhook failed") == 0);
    CHECK(state_of("hook:A") == ResourceState::kTombstone);
    CHECK(isolated_get("hook:A"));
    ResourceRuntime rt{};
    CHECK(runtime_of("hook:A", &rt));
    CHECK(rt.resolved_entry == 0x1000);
    CHECK(rt.backup == nullptr);
}

}  // namespace

int main() {
    test_install_success_sequence();
    test_install_second_fails_rollback();
    test_rollback_unhook_failure_isolates();
    test_resumable_false_skips_unhook();
    test_restore_waits_for_in_flight();
    test_restore_in_flight_timeout();
    test_isolated_blocks_begin();
    test_same_plugin_concurrent_rejected();
    test_install_live_is_idempotent_skip();
    test_install_null_backup_unhook_success_soft_fail();
    test_install_null_backup_unhook_fail_hard_even_fail_soft();
    test_query_terminal_and_reset();
    test_install_anchor_failures();
    test_release_anchors();
    test_call_patch_mechanism_transaction();
    test_non_entry_backup_handover_transaction();
    test_ownership_mechanism_transaction();
    test_terminal_retention_evicts_oldest();
    test_required_alias_group_preflight_prevents_first_hook_write();
    test_alias_can_be_reused_after_successful_physical_release();
    test_isolated_tombstone_keeps_alias_target_reserved();
    test_fail_soft_middle_failure_continues();
    test_fail_soft_last_failure_still_succeeds();
    test_fail_soft_then_hard_failure_rolls_back();
    test_fail_soft_non_entry_mechanism();
    test_rollback_skips_safely_revoked_entry();
    test_rollback_releases_free_entry_with_runtime();
    test_fail_soft_preserves_mechanism_code();
    test_fail_soft_entry_hook_keeps_generic_code();
    test_tx_abort_install_stops_running_install();
    test_tx_abort_install_rejects_non_install();
    std::printf("hooks_transaction: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
