// hooks/registry.h + hooks/install.h host 单测：
// 覆盖登记/幂等/不同 owner 冲突/状态迁移/count_by_state/容量上限/reset，
// 以及入口 hook 安装引擎的失败路径、成功落状态与失败不改状态。

#include "hooks/install.h"
#include "hooks/registry.h"

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
using qol::hooks::claim_at;
using qol::hooks::claim_count;
using qol::hooks::count_by_state;
using qol::hooks::find_claim;
using qol::hooks::install_claim_entry_hook;
using qol::hooks::InstallOutcome;
using qol::hooks::register_claim;
using qol::hooks::registry_reset_for_test;
using qol::hooks::runtime_clear;
using qol::hooks::runtime_set;
using qol::hooks::set_state;
using qol::hooks::state_of;
using qol::hooks::entry_target_conflicts;

namespace {

int g_replacement = 0;

ResourceClaim make_claim(const char* id, const char* owner,
                         ResourceMechanism mechanism = ResourceMechanism::kEntryHook,
                         void* handler = &g_replacement) {
    ResourceClaim claim{};
    claim.id = id;
    claim.symbol = "F_CHAR_ADD_DAMAGE_VMA";
    claim.mechanism = mechanism;
    claim.policy = ResourcePolicy::kPrivate;
    claim.requirement = ResourceRequirement::kRequired;
    claim.failure = ResourceFailure::kFailClosed;
    claim.resumable = true;
    claim.owner = owner;
    claim.reason = "test";
    claim.gate = "testEnabled";
    claim.step_budget = 120;
    claim.handler = handler;
    return claim;
}

// 假 hook 原语：记录调用参数并写出 backup。
struct HookRecord {
    void* target;
    void* replacement;
    void** backup_out;
    int calls;
};

HookRecord g_hook{};

int hook_ok(void* target, void* replacement, void** backup) {
    g_hook = HookRecord{target, replacement, backup, g_hook.calls + 1};
    if (backup != nullptr) {
        *backup = reinterpret_cast<void*>(0x1234);
    }
    return 0;
}

int hook_fail(void* target, void* replacement, void** backup) {
    (void)target;
    (void)replacement;
    (void)backup;
    g_hook.calls += 1;
    return -1;
}

bool error_is(const InstallOutcome& outcome, const char* expected) {
    return outcome.ok != 0 && outcome.error != nullptr &&
           std::strcmp(outcome.error, expected) == 0;
}

void test_register_and_query() {
    registry_reset_for_test();
    const ResourceClaim claim = make_claim("hook:A", "plugin_a");
    CHECK(register_claim(claim));
    CHECK(find_claim("hook:A") != nullptr);
    CHECK(find_claim("hook:A")->handler == claim.handler);
    CHECK(std::strcmp(find_claim("hook:A")->owner, "plugin_a") == 0);
    CHECK(state_of("hook:A") == ResourceState::kFree);
    CHECK(claim_count() == 1);
    CHECK(claim_at(0) != nullptr);
    CHECK(claim_at(1) == nullptr);
    CHECK(find_claim("hook:missing") == nullptr);
    CHECK(find_claim(nullptr) == nullptr);
    CHECK(state_of("hook:missing") == ResourceState::kFree);
}

void test_idempotent_and_conflict() {
    registry_reset_for_test();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    CHECK(register_claim(make_claim("hook:A", "plugin_a"))); // 同 owner 幂等
    CHECK(claim_count() == 1);
    CHECK(!register_claim(make_claim("hook:A", "plugin_b"))); // 不同 owner 冲突
    CHECK(claim_count() == 1);
    CHECK(std::strcmp(find_claim("hook:A")->owner, "plugin_a") == 0); // 保留首次登记
}

void test_invalid_declarations() {
    registry_reset_for_test();
    ResourceClaim claim = make_claim("hook:A", "plugin_a");
    claim.id = nullptr;
    CHECK(!register_claim(claim));

    claim = make_claim("hook:A", "plugin_a");
    claim.symbol = nullptr;
    CHECK(!register_claim(claim));

    claim = make_claim("hook:A", "plugin_a");
    claim.owner = nullptr;
    CHECK(!register_claim(claim));

    CHECK(claim_count() == 0);
    CHECK(!set_state("hook:A", ResourceState::kLive)); // 未登记
    CHECK(!set_state(nullptr, ResourceState::kLive));
}

void test_state_transitions_and_count() {
    registry_reset_for_test();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    CHECK(register_claim(make_claim("hook:B", "plugin_b")));
    CHECK(count_by_state(ResourceState::kFree) == 2);

    CHECK(set_state("hook:A", ResourceState::kDisabled));
    CHECK(state_of("hook:A") == ResourceState::kDisabled);
    CHECK(set_state("hook:A", ResourceState::kLive));
    CHECK(set_state("hook:B", ResourceState::kLive));
    CHECK(count_by_state(ResourceState::kLive) == 2);
    CHECK(count_by_state(ResourceState::kFree) == 0);

    CHECK(set_state("hook:B", ResourceState::kTombstone));
    CHECK(state_of("hook:B") == ResourceState::kTombstone);
    CHECK(count_by_state(ResourceState::kTombstone) == 1);
    CHECK(count_by_state(ResourceState::kLive) == 1);
}

void test_entry_target_conflict_tracks_occupied_runtime_targets() {
    registry_reset_for_test();
    ResourceClaim first = make_claim("hook:entry:first", "plugin_a");
    ResourceClaim alias = make_claim("hook:entry:alias", "plugin_b");
    ResourceClaim patch = make_claim("patch:unrelated", "plugin_c", ResourceMechanism::kCallPatch);
    CHECK(register_claim(first));
    CHECK(register_claim(alias));
    CHECK(register_claim(patch));
    CHECK(!entry_target_conflicts(0x1000, "hook:entry:first"));
    CHECK(runtime_set(first.id, 0x1000, reinterpret_cast<void*>(0x2000)));
    CHECK(set_state(first.id, ResourceState::kLive));
    CHECK(entry_target_conflicts(0x1000, alias.id));
    CHECK(!entry_target_conflicts(0x1000, first.id));
    CHECK(!entry_target_conflicts(0x1000, patch.id));
    CHECK(set_state(first.id, ResourceState::kDisabled));
    CHECK(entry_target_conflicts(0x1000, alias.id));
    CHECK(set_state(first.id, ResourceState::kTombstone));
    CHECK(qol::hooks::isolated_set(first.id, true));
    CHECK(entry_target_conflicts(0x1000, alias.id));
    CHECK(runtime_clear(first.id));
    CHECK(!entry_target_conflicts(0x1000, alias.id));
}

void test_capacity() {
    registry_reset_for_test();
    static char ids[128][16];
    for (int i = 0; i < 128; ++i) {
        std::snprintf(ids[i], sizeof(ids[i]), "hook:%d", i);
        CHECK(register_claim(make_claim(ids[i], "owner")));
    }
    CHECK(claim_count() == 128);
    CHECK(!register_claim(make_claim("hook:overflow", "owner")));
    CHECK(claim_count() == 128);
    // 满容时同 id 幂等仍成立，不误判为冲突。
    CHECK(register_claim(make_claim(ids[0], "owner")));
    CHECK(claim_count() == 128);
}

void test_reset() {
    registry_reset_for_test();
    CHECK(register_claim(make_claim("hook:A", "plugin_a")));
    CHECK(claim_count() == 1);
    registry_reset_for_test();
    CHECK(claim_count() == 0);
    CHECK(find_claim("hook:A") == nullptr);
    CHECK(state_of("hook:A") == ResourceState::kFree);
}

void test_install_success() {
    registry_reset_for_test();
    g_hook = HookRecord{};
    const ResourceClaim claim = make_claim("hook:A", "plugin_a");
    CHECK(register_claim(claim));

    const InstallOutcome live = install_claim_entry_hook(claim, 0x1000, hook_ok, true);
    CHECK(live.ok == 0);
    CHECK(live.error == nullptr);
    CHECK(live.backup == reinterpret_cast<void*>(0x1234));
    CHECK(g_hook.calls == 1);
    CHECK(g_hook.target == reinterpret_cast<void*>(0x1000));
    CHECK(g_hook.replacement == claim.handler);
    CHECK(state_of("hook:A") == ResourceState::kLive);

    const InstallOutcome disabled = install_claim_entry_hook(claim, 0x1000, hook_ok, false);
    CHECK(disabled.ok == 0);
    CHECK(state_of("hook:A") == ResourceState::kDisabled);
}

void test_install_not_registered() {
    registry_reset_for_test();
    g_hook = HookRecord{};
    const ResourceClaim claim = make_claim("hook:A", "plugin_a");
    const InstallOutcome outcome = install_claim_entry_hook(claim, 0x1000, hook_ok, true);
    CHECK(error_is(outcome, "claim not registered"));
    CHECK(g_hook.calls == 0);
    CHECK(outcome.backup == nullptr);
}

void test_install_mechanism_mismatch() {
    registry_reset_for_test();
    g_hook = HookRecord{};
    const ResourceClaim claim =
        make_claim("hook:A", "plugin_a", ResourceMechanism::kCallPatch);
    CHECK(register_claim(claim));
    const InstallOutcome outcome = install_claim_entry_hook(claim, 0x1000, hook_ok, true);
    CHECK(error_is(outcome, "mechanism mismatch"));
    CHECK(g_hook.calls == 0);
    CHECK(state_of("hook:A") == ResourceState::kFree);
}

void test_install_handler_missing() {
    registry_reset_for_test();
    g_hook = HookRecord{};
    const ResourceClaim claim =
        make_claim("hook:A", "plugin_a", ResourceMechanism::kEntryHook, nullptr);
    CHECK(register_claim(claim));
    const InstallOutcome outcome = install_claim_entry_hook(claim, 0x1000, hook_ok, true);
    CHECK(error_is(outcome, "handler missing"));
    CHECK(g_hook.calls == 0);
}

void test_install_not_ready() {
    registry_reset_for_test();
    g_hook = HookRecord{};
    const ResourceClaim claim = make_claim("hook:A", "plugin_a");
    CHECK(register_claim(claim));

    const InstallOutcome zero_entry = install_claim_entry_hook(claim, 0, hook_ok, true);
    CHECK(error_is(zero_entry, "not ready"));
    const InstallOutcome null_fn = install_claim_entry_hook(claim, 0x1000, nullptr, true);
    CHECK(error_is(null_fn, "not ready"));
    CHECK(g_hook.calls == 0);
}

void test_install_native_failure_keeps_state() {
    registry_reset_for_test();
    g_hook = HookRecord{};
    const ResourceClaim claim = make_claim("hook:A", "plugin_a");
    CHECK(register_claim(claim));
    CHECK(set_state("hook:A", ResourceState::kDisabled)); // 安装前状态

    const InstallOutcome outcome = install_claim_entry_hook(claim, 0x1000, hook_fail, true);
    CHECK(error_is(outcome, "native hook failed"));
    CHECK(outcome.backup == nullptr);
    CHECK(g_hook.calls == 1);
    CHECK(state_of("hook:A") == ResourceState::kDisabled); // 失败不改登记表状态
}

void test_resource_state_name() {
    using qol::contracts::resource_state_name;
    CHECK(std::strcmp(resource_state_name(ResourceState::kFree), "free") == 0);
    CHECK(std::strcmp(resource_state_name(ResourceState::kDisabled), "disabled") == 0);
    CHECK(std::strcmp(resource_state_name(ResourceState::kLive), "live") == 0);
    CHECK(std::strcmp(resource_state_name(ResourceState::kTombstone), "tombstone") == 0);
    CHECK(std::strcmp(resource_state_name(static_cast<ResourceState>(99)), "unknown") == 0);
}

}  // namespace

int main() {
    test_register_and_query();
    test_idempotent_and_conflict();
    test_invalid_declarations();
    test_state_transitions_and_count();
    test_entry_target_conflict_tracks_occupied_runtime_targets();
    test_capacity();
    test_reset();
    test_install_success();
    test_install_not_registered();
    test_install_mechanism_mismatch();
    test_install_handler_missing();
    test_install_not_ready();
    test_install_native_failure_keeps_state();
    test_resource_state_name();
    std::printf("hooks_registry: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
