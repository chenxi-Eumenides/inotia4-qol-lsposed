// host/plugin_registry.h host 单测（fake 后端，脱离游戏进程）：
// 覆盖成功安装、门禁关闭、部分失败回滚、回滚失败 tombstone、重复 id、
// 缺 requires 阻断与注册表 reset。资源安装原语与符号解析均由 HostDeps 注入 fake。

#include "host/plugin_registry.h"
#include "host/item_count_facade.h"

namespace qol::game {
void test_set_item_port_version(::QolVersion version);
}

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/plugin.h"
#include "contracts/game_ports/move_merge_port.h"
#include "contracts/character_state.h"
#include "contracts/item_state.h"
#include "contracts/ownership.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"
#include "infra/capabilities.h"
#include "infra/config.h"
#include "infra/events.h"
#include "infra/status.h"
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
    const char* unresolved_symbol = nullptr;
    const char* alias_symbol_a = nullptr;
    const char* alias_symbol_b = nullptr;
    uintptr_t alias_target = 0;
    const char* last_symbol = nullptr;
    const char* resolved_symbols[128] = {};
    uint32_t resolved_symbol_count = 0;
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
// 指针槽（kPtrSlot）测试夹具：假原语直接写测试槽，站点经 test_stubs 的
// save_slot_delete_hook_target 可控返回。
// ---------------------------------------------------------------------------

int g_ptr_slot_wrapper = 0;        // 被写入槽的 wrapper（与 expected 区分）
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

// 构造可控站点：槽值 = expected，登记名匹配 game/access/ptr_slots 的 kSlots 条目。
void arm_ptr_slot(void* expected) {
    g_ptr_slot_storage = expected;
    qol::game::g_ptr_slot_test_addr = &g_ptr_slot_storage;
    qol::game::g_ptr_slot_test_expected = reinterpret_cast<uintptr_t>(expected);
    qol::game::g_ptr_slot_test_resolvable = true;
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

// required × fail-soft + gate：安装失败应保持 free/degraded、可重试（§8，门禁路径）。
ResourceClaim ownership_soft_claim(const char* id, const char* owner, const char* gate) {
    ResourceClaim c = ownership_claim(id, owner);
    c.failure = ResourceFailure::kFailSoft;
    c.gate = gate;
    return c;
}

// kPtrSlot 资源：symbol 必须等于 game/access/ptr_slots 的登记名；handler 为写入槽的 wrapper。
ResourceClaim ptr_slot_claim(const char* id, const char* owner) {
    ResourceClaim c{};
    c.id = id;
    c.symbol = "ptrslot:SAVESLOT_DELETE_GOT";
    c.mechanism = ResourceMechanism::kPtrSlot;
    c.policy = ResourcePolicy::kPrivate;
    c.requirement = ResourceRequirement::kRequired;
    c.failure = ResourceFailure::kFailClosed;
    c.resumable = true;
    c.owner = owner;
    c.reason = "test";
    c.gate = nullptr;
    c.step_budget = 120;
    c.handler = &g_ptr_slot_wrapper;
    return c;
}

ResourceClaim ptr_slot_gate_claim(const char* id, const char* owner, const char* gate) {
    ResourceClaim c = ptr_slot_claim(id, owner);
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

ResourceClaim g_preflight_bad_resources[] = {
    claim("hook:preflight:bad:a", "preflight_bad", nullptr),
    claim("hook:preflight:bad:b", "preflight_bad", nullptr),
};
ResourceClaim g_preflight_good_resources[] = {
    claim("hook:preflight:good", "preflight_good", nullptr),
};
int g_preflight_capability_iface = 0;
const qol::contracts::CapabilityProvisionRef g_preflight_provides[] = {
    {"cap:test.preflight", QolVersion{1, 0}, &g_preflight_capability_iface},
};
const CapabilityRequirement g_preflight_requires[] = {
    {"cap:test.preflight", QolVersionRange{1, 0}},
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

// 常驻（gate==nullptr）fail-soft：安装失败置 retry_pending，sync_gates 定向重试；
// 未置位成功资源不得重装。
ResourceClaim g_own_resident_soft_resources[] = {
    ownership_soft_claim("own:resident:soft", "test_own_resident", nullptr),
};
ResourceClaim g_own_resident_ok_resources[] = {
    ownership_claim("own:resident:ok", "test_own_resident_ok"),
};

// 常驻 fail-soft 入口 hook：重试须重装 hook 并交还 backup（wrapper 依赖 backup）。
ResourceClaim resident_soft_hook(const char* id, const char* owner) {
    ResourceClaim c = claim(id, owner, nullptr);
    c.requirement = ResourceRequirement::kRequired;
    c.failure = ResourceFailure::kFailSoft;
    return c;
}

ResourceClaim atomic_claim(const char* id, const char* owner, const char* gate = nullptr) {
    ResourceClaim c = claim(id, owner, gate);
    c.atomic = true;
    return c;
}

ResourceClaim g_atomic_first[] = {
    atomic_claim("hook:test:atomic:first:a", "test_atomic_first"),
    atomic_claim("hook:test:atomic:first:b", "test_atomic_first"),
    atomic_claim("hook:test:atomic:first:c", "test_atomic_first"),
};
ResourceClaim g_atomic_middle[] = {
    atomic_claim("hook:test:atomic:middle:a", "test_atomic_middle"),
    atomic_claim("hook:test:atomic:middle:b", "test_atomic_middle"),
    atomic_claim("hook:test:atomic:middle:c", "test_atomic_middle"),
};
ResourceClaim g_atomic_live[] = {
    atomic_claim("hook:test:atomic:live:a", "test_atomic_live"),
    atomic_claim("hook:test:atomic:live:b", "test_atomic_live"),
    atomic_claim("hook:test:atomic:live:c", "test_atomic_live"),
};
ResourceClaim g_atomic_retry[] = {
    atomic_claim("hook:test:atomic:retry:a", "test_atomic_retry"),
    atomic_claim("hook:test:atomic:retry:b", "test_atomic_retry"),
    atomic_claim("hook:test:atomic:retry:c", "test_atomic_retry"),
};
ResourceClaim g_atomic_visible[] = {
    atomic_claim("hook:test:atomic:visible:a", "test_atomic_visible"),
    atomic_claim("hook:test:atomic:visible:b", "test_atomic_visible"),
};
ResourceClaim g_atomic_gate[] = {
    atomic_claim("hook:test:atomic:gate:a", "test_atomic_gate", "test.atomic.gate"),
    atomic_claim("hook:test:atomic:gate:b", "test_atomic_gate", "test.atomic.gate"),
};

int g_atomic_capability_iface = 0;
const qol::contracts::CapabilityProvisionRef g_atomic_provides[] = {
    {"cap:test.atomic", QolVersion{1, 0}, &g_atomic_capability_iface},
};
ResourceClaim g_resident_soft_hook[] = {
    resident_soft_hook("hook:test:resident:hook", "test_resident_hook"),
};

// 常驻重试成功后，后续必需 fail-closed 资源失败：前者必须纳入同一回滚列表。
ResourceClaim g_resident_retry_then_fail[] = {
    resident_soft_hook("hook:test:resident:rollback", "test_resident_rollback"),
    ptr_slot_gate_claim("ptrslot:test:resident:rollback", "test_resident_rollback",
                        "test.resident.rollback"),
};

// kPtrSlot 分派：单条指针槽资源，成功安装后槽值为 wrapper，backup 交还。
ResourceClaim g_ptr_slot_resources[] = {
    ptr_slot_claim("ptrslot:test:a", "test_ptrslot"),
};

// kPtrSlot 回滚：指针槽先装成功，随后的入口 hook 失败 → 逆序归还指针槽（写回 expected）。
ResourceClaim g_ptr_slot_release_resources[] = {
    ptr_slot_claim("ptrslot:test:rb", "test_ptrslot_rb"),
    claim("hook:test:ptr:rb", "test_ptrslot_rb", nullptr),
};

// R1：optional 软边拓扑夹具（测试本地声明 provides/optional，不改插件 descriptor）。
const qol::contracts::CapabilityProvisionRef g_item_facts_provides[] = {
    {"item.facts", QolVersion{1, 0}, &g_dummy_handler},
};
const qol::contracts::CapabilityProvisionRef g_eb_api_provides[] = {
    {"eb.api", QolVersion{1, 0}, &g_dummy_handler},
};
const qol::contracts::CapabilityProvisionRef g_b_api_provides[] = {
    {"b.api", QolVersion{1, 0}, &g_dummy_handler},
};
const qol::contracts::CapabilityProvisionRef g_x_api_provides[] = {
    {"x.api", QolVersion{1, 0}, &g_dummy_handler},
};
const qol::contracts::CapabilityProvisionRef g_y_api_provides[] = {
    {"y.api", QolVersion{1, 0}, &g_dummy_handler},
};
const CapabilityRequirement g_item_facts_requires[] = {
    {"item.facts", QolVersionRange{1, 0}},
};
const CapabilityRequirement g_x_api_requires[] = {
    {"x.api", QolVersionRange{1, 0}},
};
const CapabilityRequirement g_y_api_requires[] = {
    {"y.api", QolVersionRange{1, 0}},
};
const CapabilityRequirement g_eb_api_optional[] = {
    {"eb.api", QolVersionRange{1, 0}},
};
const qol::contracts::CapabilityProvisionRef g_character_state_provides[] = {
    {qol::contracts::kCharacterStateCapabilityId, qol::contracts::kCharacterStateVersion,
     &g_dummy_handler},
};
const qol::contracts::CapabilityProvisionRef g_item_state_provides[] = {
    {qol::contracts::kItemStateCapabilityId, qol::contracts::kItemStateVersion,
     &g_dummy_handler},
};
const CapabilityRequirement g_item_state_optional[] = {
    {qol::contracts::kItemStateCapabilityId,
     {qol::contracts::kItemStateVersion.major, qol::contracts::kItemStateVersion.minor}},
};
const CapabilityRequirement g_character_state_optional[] = {
    {qol::contracts::kCharacterStateCapabilityId,
     {qol::contracts::kCharacterStateVersion.major,
      qol::contracts::kCharacterStateVersion.minor}},
};
const CapabilityRequirement g_b_api_optional[] = {
    {"b.api", QolVersionRange{1, 0}},
};

char g_dep_order[64] = {};
int dep_install_a(PluginContext&) {
    ++g_probe.install_calls;
    std::strcat(g_dep_order, "A");
    return 0;
}
int dep_install_b(PluginContext&) {
    ++g_probe.install_calls;
    std::strcat(g_dep_order, "B");
    return 0;
}
int dep_install_fail(PluginContext&) {
    ++g_probe.install_calls;
    return 1;
}

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

// R1：带 capability 依赖的 descriptor（install 由用例指定，便于断言拓扑顺序）。
PluginDescriptor make_dep_descriptor(const char* id, const CapabilityRequirement* requires,
                                     uint32_t requires_count,
                                     const CapabilityRequirement* optional, uint32_t optional_count,
                                     const qol::contracts::CapabilityProvisionRef* provides,
                                     uint32_t provides_count,
                                     int (*install_fn)(PluginContext&),
                                     const ResourceClaim* resources = nullptr,
                                     uint32_t resource_count = 0) {
    PluginDescriptor d = make_descriptor(id, resources, resource_count, requires, requires_count);
    d.optional_count = optional_count;
    d.optional_requires = optional;
    d.provides_count = provides_count;
    d.provides = provides;
    d.install = install_fn;
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
    g_dep_order[0] = '\0';
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

void test_choice_port_version_gate() {
    reset_all();
    qol::contracts::QolPortRequirement requirement{
        qol::contracts::QolPortId::QOL_PORT_CHOICE,
        {qol::contracts::kQolPortChoiceVersion.major,
         qol::contracts::kQolPortChoiceVersion.minor},
    };
    PluginDescriptor valid = make_descriptor("core.custom_ui", nullptr, 0, nullptr, 0);
    valid.ports = &requirement;
    valid.port_count = 1;
    CHECK(register_plugin(valid));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.custom_ui", FeatureState::kEnabled));

    reset_all();
    requirement.range.min_minor = static_cast<uint16_t>(
        qol::contracts::kQolPortChoiceVersion.minor + 1);
    PluginDescriptor incompatible = make_descriptor("core.custom_ui", nullptr, 0, nullptr, 0);
    incompatible.ports = &requirement;
    incompatible.port_count = 1;
    CHECK(register_plugin(incompatible));
    CHECK(install_all(make_deps()) == 1);
    const char* error = nullptr;
    CHECK(feature_state_is("core.custom_ui", FeatureState::kBlocked));
    CHECK(feature_of("core.custom_ui", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "unsupported port version") != nullptr);
}

// Item Port 3.3 ABI 要求拒绝 3.2 及更早适配器。
void test_item_port_v33_rejects_legacy_adapters() {
    reset_all();
    qol::contracts::QolPortRequirement requirement{
        qol::contracts::QolPortId::QOL_PORT_ITEM,
        {qol::contracts::kQolPortItemVersion.major, qol::contracts::kQolPortItemVersion.minor}};
    const ::QolVersion legacy_versions[] = {{1, 1}, {1, 2}, {3, 2}};
    for (const auto legacy : legacy_versions) {
        reset_all();
        PluginDescriptor descriptor = make_descriptor("core.item", nullptr, 0, nullptr, 0);
        descriptor.ports = &requirement;
        descriptor.port_count = 1;
        qol::game::test_set_item_port_version(legacy);
        CHECK(register_plugin(descriptor));
        CHECK(install_all(make_deps()) == 1);
        const char* error = nullptr;
        CHECK(feature_state_is("core.item", FeatureState::kBlocked));
        CHECK(feature_of("core.item", nullptr, nullptr, &error));
        CHECK(error != nullptr && std::strstr(error, "unsupported port version") != nullptr);
    }
    qol::game::test_set_item_port_version(qol::contracts::kQolPortItemVersion);
}

void test_save_port_version_and_private_gate() {
    reset_all();
    qol::contracts::QolPortRequirement requirement{
        qol::contracts::QolPortId::QOL_PORT_SAVE,
        {qol::contracts::kQolPortSaveVersion.major, qol::contracts::kQolPortSaveVersion.minor},
    };
    PluginDescriptor valid = make_descriptor("core.save", nullptr, 0, nullptr, 0);
    valid.ports = &requirement;
    valid.port_count = 1;
    CHECK(register_plugin(valid));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.save", FeatureState::kEnabled));

    reset_all();
    requirement.range.min_minor = static_cast<uint16_t>(
        qol::contracts::kQolPortSaveVersion.minor + 1);
    PluginDescriptor incompatible = make_descriptor("core.save", nullptr, 0, nullptr, 0);
    incompatible.ports = &requirement;
    incompatible.port_count = 1;
    CHECK(register_plugin(incompatible));
    CHECK(install_all(make_deps()) == 1);
    const char* error = nullptr;
    CHECK(feature_state_is("core.save", FeatureState::kBlocked));
    CHECK(feature_of("core.save", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "unsupported port version") != nullptr);

    reset_all();
    requirement.id = qol::contracts::QolPortId::QOL_PORT_SAVE_CALL;
    requirement.range.min_minor = qol::contracts::kQolPortSaveCallVersion.minor;
    PluginDescriptor private_port = make_descriptor("test_save", nullptr, 0, nullptr, 0);
    private_port.ports = &requirement;
    private_port.port_count = 1;
    CHECK(register_plugin(private_port));
    CHECK(install_all(make_deps()) == 1);
    CHECK(feature_state_is("test_save", FeatureState::kBlocked));
    CHECK(feature_of("test_save", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "private port") != nullptr);
}

void test_craft_port_version_and_private_gate() {
    reset_all();
    qol::contracts::QolPortRequirement requirement{
        qol::contracts::QolPortId::QOL_PORT_CRAFT,
        {qol::contracts::kQolPortCraftVersion.major, qol::contracts::kQolPortCraftVersion.minor},
    };
    PluginDescriptor valid = make_descriptor("core.craft", nullptr, 0, nullptr, 0);
    valid.ports = &requirement;
    valid.port_count = 1;
    CHECK(register_plugin(valid));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.craft", FeatureState::kEnabled));

    reset_all();
    requirement.range.min_minor = static_cast<uint16_t>(
        qol::contracts::kQolPortCraftVersion.minor + 1);
    PluginDescriptor incompatible = make_descriptor("core.craft", nullptr, 0, nullptr, 0);
    incompatible.ports = &requirement;
    incompatible.port_count = 1;
    CHECK(register_plugin(incompatible));
    CHECK(install_all(make_deps()) == 1);
    const char* error = nullptr;
    CHECK(feature_state_is("core.craft", FeatureState::kBlocked));
    CHECK(feature_of("core.craft", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "unsupported port version") != nullptr);

    reset_all();
    requirement.range.min_minor = qol::contracts::kQolPortCraftVersion.minor;
    PluginDescriptor private_port = make_descriptor("test_craft", nullptr, 0, nullptr, 0);
    private_port.ports = &requirement;
    private_port.port_count = 1;
    CHECK(register_plugin(private_port));
    CHECK(install_all(make_deps()) == 1);
    CHECK(feature_state_is("test_craft", FeatureState::kBlocked));
    CHECK(feature_of("test_craft", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "private port") != nullptr);
}

void test_character_http_port_is_owner_private() {
    reset_all();
    qol::contracts::QolPortRequirement requirement{
        qol::contracts::QolPortId::QOL_PORT_CHARACTER_HTTP,
        {qol::contracts::kQolPortCharacterHttpVersion.major,
         qol::contracts::kQolPortCharacterHttpVersion.minor},
    };
    PluginDescriptor private_port = make_descriptor("test_character", nullptr, 0, nullptr, 0);
    private_port.ports = &requirement;
    private_port.port_count = 1;
    CHECK(register_plugin(private_port));
    CHECK(install_all(make_deps()) == 1);
    CHECK(feature_state_is("test_character", FeatureState::kBlocked));
    const char* error = nullptr;
    CHECK(feature_of("test_character", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "private port") != nullptr);

    reset_all();
    PluginDescriptor owner = make_descriptor("core.character", nullptr, 0, nullptr, 0);
    owner.ports = &requirement;
    owner.port_count = 1;
    CHECK(register_plugin(owner));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.character", FeatureState::kEnabled));
}

void test_ui_state_read_port_is_owner_private() {
    reset_all();
    qol::contracts::QolPortRequirement requirement{
        qol::contracts::QolPortId::QOL_PORT_UI_STATE_READ,
        {qol::contracts::kQolPortUiStateReadVersion.major,
         qol::contracts::kQolPortUiStateReadVersion.minor},
    };
    PluginDescriptor private_port = make_descriptor("test_ui_state", nullptr, 0, nullptr, 0);
    private_port.ports = &requirement;
    private_port.port_count = 1;
    CHECK(register_plugin(private_port));
    CHECK(install_all(make_deps()) == 1);
    CHECK(feature_state_is("test_ui_state", FeatureState::kBlocked));
    const char* error = nullptr;
    CHECK(feature_of("test_ui_state", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "private port") != nullptr);

    reset_all();
    PluginDescriptor owner = make_descriptor("core.ui_state", nullptr, 0, nullptr, 0);
    owner.ports = &requirement;
    owner.port_count = 1;
    CHECK(register_plugin(owner));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.ui_state", FeatureState::kEnabled));
}

void test_move_merge_port_is_owner_private() {
    reset_all();
    qol::contracts::QolPortRequirement requirement{
        qol::contracts::QolPortId::QOL_PORT_MOVE_MERGE,
        {qol::contracts::kQolMoveMergePortVersion.major,
         qol::contracts::kQolMoveMergePortVersion.minor},
    };
    PluginDescriptor private_port = make_descriptor("test_move", nullptr, 0, nullptr, 0);
    private_port.ports = &requirement;
    private_port.port_count = 1;
    CHECK(register_plugin(private_port));
    CHECK(install_all(make_deps()) == 1);
    CHECK(feature_state_is("test_move", FeatureState::kBlocked));
    const char* error = nullptr;
    CHECK(feature_of("test_move", nullptr, nullptr, &error));
    CHECK(error != nullptr && std::strstr(error, "private port") != nullptr);

    reset_all();
    PluginDescriptor owner = make_descriptor("move_merge", nullptr, 0, nullptr, 0);
    owner.ports = &requirement;
    owner.port_count = 1;
    CHECK(register_plugin(owner));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("move_merge", FeatureState::kEnabled));
}

void test_gate_off_installed_disabled() {    reset_all();
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
    CHECK(qol::hooks::isolated_get("hook:test:fail:a"));
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

// kPtrSlot 分派：站点经 game::ptr_slot_site 解析，槽值写入 wrapper，资源 live；
// backup（槽期望值 = 原函数入口）经 on_resource_ready 交还所有者（§3.3）。
void test_ptr_slot_dispatch_live_and_backup_handover() {
    reset_all();
    arm_ptr_slot(&g_dummy_handler);
    const PluginDescriptor d =
        make_descriptor("test_ptrslot", g_ptr_slot_resources, 1, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 0);
    CHECK(g_backend.hook_calls == 0);
    CHECK(g_ptr_slot_storage == &g_ptr_slot_wrapper);
    CHECK(qol::hooks::state_of("ptrslot:test:a") == ResourceState::kLive);
    CHECK(feature_state_is("test_ptrslot", FeatureState::kEnabled));
    CHECK(g_probe.ready_calls == 1);
    CHECK(g_probe.ready_backups[0] == &g_dummy_handler);
}

// kPtrSlot 回滚：指针槽先装成功，随后的入口 hook 失败 → 逆序归还指针槽
// （写回 expected、释放占用），插件 failed，无半装残留。
void test_ptr_slot_rollback_releases_slot() {
    reset_all();
    arm_ptr_slot(&g_dummy_handler);
    g_backend.fail_hook_call = 1;  // 第二条（入口 hook）安装失败
    const PluginDescriptor d =
        make_descriptor("test_ptrslot_rb", g_ptr_slot_release_resources, 2, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 1);
    CHECK(g_ptr_slot_storage == &g_dummy_handler);
    CHECK(qol::patch::point_owner(reinterpret_cast<uintptr_t>(&g_ptr_slot_storage)) == nullptr);
    CHECK(qol::hooks::state_of("ptrslot:test:rb") == ResourceState::kFree);
    CHECK(qol::patch::claimed_count() == 0);
    CHECK(feature_state_is("test_ptrslot_rb", FeatureState::kFailed));
}

// kPtrSlot 槽值不符：fail-closed 拒绝安装，不写槽、不占点、插件 failed。
void test_ptr_slot_value_mismatch_fail_closed() {
    reset_all();
    arm_ptr_slot(&g_dummy_handler);
    g_ptr_slot_storage = &g_ptr_slot_wrapper;  // 槽当前值 != expected
    const PluginDescriptor d =
        make_descriptor("test_ptrslot", g_ptr_slot_resources, 1, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 1);
    CHECK(g_ptr_slot_storage == &g_ptr_slot_wrapper);  // 未被改写
    CHECK(qol::patch::point_owner(reinterpret_cast<uintptr_t>(&g_ptr_slot_storage)) == nullptr);
    CHECK(qol::hooks::state_of("ptrslot:test:a") == ResourceState::kFree);
    CHECK(!feature_degraded("test_ptrslot"));
    CHECK(feature_state_is("test_ptrslot", FeatureState::kFailed));
}

// 常驻（gate==nullptr）required×fail-soft：安装失败保持 free + degraded + retry_pending，
// 插件仍 enabled、不 tombstone/isolated；sync_gates 定向重试，成功 → live、清标记/degraded，
// 失败 → 保持 free/degraded/retry_pending。
void test_resident_fail_soft_retry() {
    reset_all();
    g_ownership.fail_acquire_at = 1;
    const PluginDescriptor d =
        make_descriptor("test_own_resident", g_own_resident_soft_resources, 1, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 0);
    CHECK(g_ownership.acquire_calls == 1);
    CHECK(qol::hooks::state_of("own:resident:soft") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("own:resident:soft"));
    CHECK(!qol::hooks::isolated_get("own:resident:soft"));
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
    CHECK(feature_state_is("test_own_resident", FeatureState::kEnabled));
    CHECK(feature_degraded("test_own_resident"));

    // 重试仍失败（第 2 次 acquire）：保持 free/degraded/retry_pending，不 tombstone/isolated。
    g_ownership.fail_acquire_at = 2;
    sync_gates();
    CHECK(g_ownership.acquire_calls == 2);
    CHECK(qol::hooks::state_of("own:resident:soft") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("own:resident:soft"));
    CHECK(feature_degraded("test_own_resident"));
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);

    // 重试成功：live、清 retry_pending/degraded，无 tombstone/isolated。
    g_ownership.fail_acquire_at = 0;
    sync_gates();
    CHECK(g_ownership.acquire_calls == 3);
    CHECK(qol::hooks::state_of("own:resident:soft") == ResourceState::kLive);
    CHECK(!qol::hooks::retry_pending_get("own:resident:soft"));
    CHECK(!feature_degraded("test_own_resident"));
    CHECK(feature_state_is("test_own_resident", FeatureState::kEnabled));
    CHECK(qol::hooks::count_by_state(ResourceState::kTombstone) == 0);
    CHECK(!qol::hooks::isolated_get("own:resident:soft"));
}

// 常驻（gate==nullptr）安装成功的资源：retry_pending 未置位，sync_gates 不得重装。
void test_resident_success_not_reinstalled() {
    reset_all();
    const PluginDescriptor d =
        make_descriptor("test_own_resident_ok", g_own_resident_ok_resources, 1, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(g_ownership.acquire_calls == 1);
    CHECK(qol::hooks::state_of("own:resident:ok") == ResourceState::kLive);
    CHECK(!qol::hooks::retry_pending_get("own:resident:ok"));

    CHECK(sync_gates() == 0);
    CHECK(g_ownership.acquire_calls == 1);  // 调用计数不变
    CHECK(qol::hooks::state_of("own:resident:ok") == ResourceState::kLive);
    CHECK(!qol::hooks::retry_pending_get("own:resident:ok"));
}

// 常驻 fail-soft 入口 hook：安装失败 free + retry_pending；sync_gates 重装成功并交还 backup。
void test_resident_entry_hook_fail_soft_retry() {
    reset_all();
    g_backend.fail_hook_call = 1;
    const PluginDescriptor d =
        make_descriptor("test_resident_hook", g_resident_soft_hook, 1, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 0);
    CHECK(g_backend.hook_calls == 1);
    CHECK(qol::hooks::state_of("hook:test:resident:hook") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("hook:test:resident:hook"));
    CHECK(g_probe.ready_calls == 0);
    CHECK(feature_state_is("test_resident_hook", FeatureState::kEnabled));
    CHECK(feature_degraded("test_resident_hook"));

    // 定向重试成功：重装 hook、交还 backup、清 retry_pending/degraded。
    g_backend.fail_hook_call = 0;
    sync_gates();
    CHECK(g_backend.hook_calls == 2);
    CHECK(qol::hooks::state_of("hook:test:resident:hook") == ResourceState::kLive);
    CHECK(!qol::hooks::retry_pending_get("hook:test:resident:hook"));
    CHECK(g_probe.ready_calls == 1);
    CHECK(!feature_degraded("test_resident_hook"));
    CHECK(feature_state_is("test_resident_hook", FeatureState::kEnabled));

    // 已恢复：再次同步不重装。
    sync_gates();
    CHECK(g_backend.hook_calls == 2);
    CHECK(g_probe.ready_calls == 1);
}

void test_resident_retry_is_rolled_back_with_later_failure() {
    reset_all();
    qol::infra::config_set_bool("test.resident.rollback", false);
    g_backend.fail_hook_call = 1;
    arm_ptr_slot(&g_dummy_handler);
    const PluginDescriptor d =
        make_descriptor("test_resident_rollback", g_resident_retry_then_fail, 2, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 0);
    CHECK(qol::hooks::state_of("hook:test:resident:rollback") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("hook:test:resident:rollback"));
    CHECK(qol::hooks::state_of("ptrslot:test:resident:rollback") == ResourceState::kFree);

    // 常驻重试先成功；随后 ptr slot 的 required×fail-closed 安装失败。
    g_backend.fail_hook_call = 0;
    g_ptr_slot_storage = &g_ptr_slot_wrapper;
    qol::infra::config_set_bool("test.resident.rollback", true);
    CHECK(sync_gates() == 1);
    CHECK(feature_state_is("test_resident_rollback", FeatureState::kFailed));
    CHECK(g_backend.unhook_calls == 1);
    CHECK(qol::hooks::state_of("hook:test:resident:rollback") == ResourceState::kFree);
    CHECK(!qol::hooks::retry_pending_get("hook:test:resident:rollback"));
    CHECK(!qol::hooks::isolated_get("hook:test:resident:rollback"));
    CHECK(qol::hooks::state_of("ptrslot:test:resident:rollback") == ResourceState::kTombstone);
    CHECK(qol::hooks::isolated_get("ptrslot:test:resident:rollback"));
}

void test_atomic_first_member_failure_is_degraded_and_pending() {
    reset_all();
    g_backend.fail_hook_call = 1;
    const PluginDescriptor d =
        make_descriptor("test_atomic_first", g_atomic_first, 3, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("test_atomic_first", FeatureState::kInstalledDisabled));
    const char* error = nullptr;
    CHECK(feature_of("test_atomic_first", nullptr, nullptr, &error));
    CHECK(error != nullptr);
    CHECK(std::strstr(error, "hook:test:atomic:first:a") != nullptr);
    CHECK(feature_degraded("test_atomic_first"));
    CHECK(g_probe.enabled_calls == 0);
    CHECK(qol::hooks::state_of("hook:test:atomic:first:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:atomic:first:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:atomic:first:c") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:first:a"));
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:first:b"));
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:first:c"));
}

void test_atomic_middle_member_failure_rolls_back_prefix_and_stays_free() {
    reset_all();
    g_backend.fail_hook_call = 2;
    const PluginDescriptor d =
        make_descriptor("test_atomic_middle", g_atomic_middle, 3, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(g_backend.unhook_calls == 1);
    CHECK(feature_state_is("test_atomic_middle", FeatureState::kInstalledDisabled));
    CHECK(feature_degraded("test_atomic_middle"));
    CHECK(qol::hooks::state_of("hook:test:atomic:middle:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:atomic:middle:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:atomic:middle:c") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:middle:a"));
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:middle:b"));
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:middle:c"));
}

void test_atomic_failure_releases_members_live_from_previous_round() {
    reset_all();
    const PluginDescriptor d = make_descriptor("test_atomic_live", g_atomic_live, 3, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("test_atomic_live", FeatureState::kEnabled));

    CHECK(qol::hooks::set_state("hook:test:atomic:live:b", ResourceState::kFree));
    qol::hooks::runtime_clear("hook:test:atomic:live:b");
    qol::hooks::retry_pending_set("hook:test:atomic:live:b", true);
    g_backend.fail_hook_call = g_backend.hook_calls + 1;
    CHECK(sync_gates() == 1);
    CHECK(feature_state_is("test_atomic_live", FeatureState::kInstalledDisabled));
    CHECK(feature_degraded("test_atomic_live"));
    CHECK(qol::hooks::state_of("hook:test:atomic:live:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:atomic:live:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:test:atomic:live:c") == ResourceState::kFree);
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:live:a"));
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:live:b"));
    CHECK(qol::hooks::retry_pending_get("hook:test:atomic:live:c"));
}

void test_atomic_retry_success_is_enabled_once_and_clears_degraded() {
    reset_all();
    g_backend.fail_hook_call = 2;
    const PluginDescriptor d = make_descriptor("test_atomic_retry", g_atomic_retry, 3, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(g_probe.enabled_calls == 0);
    CHECK(feature_degraded("test_atomic_retry"));

    g_backend.fail_hook_call = 0;
    CHECK(sync_gates() == 1);
    CHECK(feature_state_is("test_atomic_retry", FeatureState::kEnabled));
    CHECK(!feature_degraded("test_atomic_retry"));
    CHECK(g_probe.enabled_calls == 1);
    CHECK(qol::hooks::state_of("hook:test:atomic:retry:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:test:atomic:retry:b") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:test:atomic:retry:c") == ResourceState::kLive);
    CHECK(sync_gates() == 0);
    CHECK(g_probe.enabled_calls == 1);
}

void test_atomic_partial_install_publishes_no_capability_or_facade_provider() {
    reset_all();
    g_backend.fail_hook_call = 2;
    PluginDescriptor d = make_descriptor("test_atomic_visible", g_atomic_visible, 2, nullptr, 0);
    d.provides_count = 1;
    d.provides = g_atomic_provides;
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("test_atomic_visible", FeatureState::kInstalledDisabled));
    CHECK(g_probe.enabled_calls == 0);
    CHECK(qol::infra::require_capability("cap:test.atomic", QolVersionRange{1, 0}) ==
          nullptr);
    uint32_t count = 41;
    // Host 常驻 facade 始终可查，但无目标内存时 fail-closed；原子组半装不得使扩展模式生效。
    CHECK(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &count).code ==
          kQolErrUnsupported);
    CHECK(count == 41);
    CHECK(!qol::game::item_count_active());
}

void test_atomic_gate_closed_does_not_call_on_enabled() {
    reset_all();
    qol::infra::config_set_bool("test.atomic.gate", false);
    const PluginDescriptor d = make_descriptor("test_atomic_gate", g_atomic_gate, 2, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("test_atomic_gate", FeatureState::kInstalledDisabled));
    CHECK(g_probe.enabled_calls == 0);
    CHECK(qol::hooks::state_of("hook:test:atomic:gate:a") == ResourceState::kDisabled);
    CHECK(qol::hooks::state_of("hook:test:atomic:gate:b") == ResourceState::kDisabled);

    qol::infra::config_set_bool("test.atomic.gate", true);
    CHECK(sync_gates() == 1);
    CHECK(feature_state_is("test_atomic_gate", FeatureState::kEnabled));
    CHECK(g_probe.enabled_calls == 1);
    CHECK(qol::hooks::state_of("hook:test:atomic:gate:a") == ResourceState::kLive);
    CHECK(qol::hooks::state_of("hook:test:atomic:gate:b") == ResourceState::kLive);
}

void test_required_entries_preflight_before_plugin_install() {
    reset_all();
    ResourceClaim resources[] = {claim("hook:preflight:a", "preflight", nullptr),
                                 claim("hook:preflight:b", "preflight", nullptr)};
    resources[0].symbol = "elf-export:Fixture_Export_A";
    resources[1].symbol = "elf-export:Fixture_Export_B";
    PluginDescriptor d = make_descriptor("preflight", resources, 2, nullptr, 0);
    CHECK(register_plugin(d));
    g_backend.unresolved_symbol = "elf-export:Fixture_Export_B";
    CHECK(install_all(make_deps()) == 1);
    CHECK(g_probe.install_calls == 0);
    CHECK(g_backend.hook_calls == 0);
    CHECK(std::strcmp(g_backend.last_symbol, "elf-export:Fixture_Export_B") == 0);
    CHECK(feature_state_is("preflight", FeatureState::kUnsupported));
    CHECK(qol::hooks::state_of("hook:preflight:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:preflight:b") == ResourceState::kFree);
}

void test_preflight_failure_isolated_from_independent_plugins() {
    reset_all();
    g_preflight_bad_resources[0].symbol = "elf-export:Fixture_Export_A";
    g_preflight_bad_resources[1].symbol = "elf-export:Fixture_Export_B";
    g_backend.unresolved_symbol = "elf-export:Fixture_Export_B";

    PluginDescriptor bad = make_descriptor("preflight_bad", g_preflight_bad_resources, 2,
                                            nullptr, 0);
    bad.provides_count = 1;
    bad.provides = g_preflight_provides;
    const PluginDescriptor good = make_descriptor("preflight_good", g_preflight_good_resources, 1,
                                                  nullptr, 0);
    const PluginDescriptor consumer = make_descriptor("preflight_consumer", nullptr, 0,
                                                      g_preflight_requires, 1);
    CHECK(register_plugin(bad));
    CHECK(register_plugin(good));
    CHECK(register_plugin(consumer));

    CHECK(install_all(make_deps()) == 2);  // bad unsupported + its consumer blocked
    CHECK(feature_state_is("preflight_bad", FeatureState::kUnsupported));
    CHECK(feature_state_is("preflight_good", FeatureState::kEnabled));
    CHECK(feature_state_is("preflight_consumer", FeatureState::kBlocked));
    CHECK(g_probe.install_calls == 1);  // 独立 good 插件仍安装
    CHECK(g_backend.hook_calls == 1);
    CHECK(qol::hooks::state_of("hook:preflight:bad:a") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:preflight:bad:b") == ResourceState::kFree);
    CHECK(qol::hooks::state_of("hook:preflight:good") == ResourceState::kLive);
}

void test_same_plugin_entry_aliases_fail_before_install_or_hook() {
    reset_all();
    g_backend.alias_symbol_a = "elf-export:EntryAliasA";
    g_backend.alias_symbol_b = "elf-export:EntryAliasB";
    g_backend.alias_target = 0x7000;
    ResourceClaim resources[] = {claim("hook:alias:first", "alias_plugin", nullptr),
                                 claim("hook:alias:second", "alias_plugin", nullptr)};
    resources[0].symbol = g_backend.alias_symbol_a;
    resources[1].symbol = g_backend.alias_symbol_b;
    const PluginDescriptor d = make_descriptor("alias_plugin", resources, 2, nullptr, 0);
    CHECK(register_plugin(d));

    CHECK(install_all(make_deps()) == 1);
    CHECK(feature_state_is("alias_plugin", FeatureState::kUnsupported));
    CHECK(g_probe.install_calls == 0);
    CHECK(g_backend.hook_calls == 0);
    CHECK(qol::hooks::state_of(resources[0].id) == ResourceState::kFree);
    CHECK(qol::hooks::state_of(resources[1].id) == ResourceState::kFree);
}

void test_cross_plugin_entry_alias_rejected_but_first_owner_stays_live() {
    reset_all();
    g_backend.alias_symbol_a = "elf-export:CrossAliasA";
    g_backend.alias_symbol_b = "elf-export:CrossAliasB";
    g_backend.alias_target = 0x7100;
    ResourceClaim first_resource = claim("hook:cross:first", "cross_first", nullptr);
    ResourceClaim second_resource = claim("hook:cross:second", "cross_second", nullptr);
    first_resource.symbol = g_backend.alias_symbol_a;
    second_resource.symbol = g_backend.alias_symbol_b;
    const PluginDescriptor first =
        make_descriptor("cross_first", &first_resource, 1, nullptr, 0);
    const PluginDescriptor second =
        make_descriptor("cross_second", &second_resource, 1, nullptr, 0);
    CHECK(register_plugin(first));
    CHECK(register_plugin(second));

    CHECK(install_all(make_deps()) == 1);
    CHECK(feature_state_is("cross_first", FeatureState::kEnabled));
    CHECK(feature_state_is("cross_second", FeatureState::kUnsupported));
    CHECK(g_probe.install_calls == 1);
    CHECK(g_backend.hook_calls == 1);
    CHECK(g_backend.last_target == reinterpret_cast<void*>(g_backend.alias_target));
    CHECK(qol::hooks::state_of(first_resource.id) == ResourceState::kLive);
    CHECK(qol::hooks::state_of(second_resource.id) == ResourceState::kFree);
}

void test_legacy_entry_symbol_id_installs() {
    reset_all();
    ResourceClaim resource = claim("hook:legacy-symbol", "legacy_symbol", nullptr);
    resource.symbol = "F_TEST_VMA";
    const PluginDescriptor d = make_descriptor("legacy_symbol", &resource, 1, nullptr, 0);
    CHECK(register_plugin(d));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("legacy_symbol", FeatureState::kEnabled));
    CHECK(g_backend.hook_calls == 1);
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
// R1：optional 软边两阶段拓扑 + optional 两段检查
// ---------------------------------------------------------------------------

bool feature_error_is(const char* id, const char* expected) {
    const char* error = nullptr;
    if (!feature_of(id, nullptr, nullptr, &error)) return false;
    return error != nullptr && std::strcmp(error, expected) == 0;
}

bool feature_error_lacks(const char* id, const char* needle) {
    const char* error = nullptr;
    if (!feature_of(id, nullptr, nullptr, &error)) return false;
    return error == nullptr || std::strstr(error, needle) == nullptr;
}

// ① 环最小复现：A optional eb.api + B required item.facts → 阶段 2 回退，全 enabled、A 先 B 后。
void test_optional_cycle_soft_edge() {
    reset_all();
    const PluginDescriptor a = make_dep_descriptor("alpha", nullptr, 0, g_eb_api_optional, 1,
                                                   g_item_facts_provides, 1, &dep_install_a);
    const PluginDescriptor b = make_dep_descriptor("beta", g_item_facts_requires, 1, nullptr, 0,
                                                   g_eb_api_provides, 1, &dep_install_b);
    CHECK(register_plugin(a));
    CHECK(register_plugin(b));

    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("alpha", FeatureState::kEnabled));
    CHECK(feature_state_is("beta", FeatureState::kEnabled));
    CHECK(!feature_degraded("alpha"));
    CHECK(!feature_degraded("beta"));
    CHECK(std::strcmp(g_dep_order, "AB") == 0);
    CHECK(feature_error_lacks("alpha", "dependency cycle"));
    CHECK(feature_error_lacks("beta", "dependency cycle"));
}

// ② 现实拓扑混合环：core.item(optional extension_bag) ↔ extension_bag(required item.facts)。
void test_optional_required_mixed_cycle_real_topology() {
    reset_all();
    const PluginDescriptor item = make_dep_descriptor("core.item", nullptr, 0, g_eb_api_optional, 1,
                                                      g_item_facts_provides, 1, &dep_install_a);
    const PluginDescriptor eb =
        make_dep_descriptor("extension_bag", g_item_facts_requires, 1, nullptr, 0,
                            g_eb_api_provides, 1, &dep_install_b);
    CHECK(register_plugin(item));
    CHECK(register_plugin(eb));

    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.item", FeatureState::kEnabled));
    CHECK(feature_state_is("extension_bag", FeatureState::kEnabled));
    CHECK(!feature_degraded("core.item"));
    CHECK(!feature_degraded("extension_bag"));
    CHECK(std::strcmp(g_dep_order, "AB") == 0);
    CHECK(feature_error_lacks("core.item", "dependency cycle"));
}

void test_core_item_optional_character_state_degradation() {
    reset_all();
    PluginDescriptor item_missing = make_dep_descriptor(
        "core.item", nullptr, 0, g_character_state_optional, 1, nullptr, 0, &dep_install_a);
    CHECK(register_plugin(item_missing));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.item", FeatureState::kEnabled));
    CHECK(feature_degraded("core.item"));

    reset_all();
    PluginDescriptor item_present = make_dep_descriptor(
        "core.item", nullptr, 0, g_character_state_optional, 1, nullptr, 0, &dep_install_a);
    PluginDescriptor character = make_dep_descriptor(
        "core.character", nullptr, 0, nullptr, 0, g_character_state_provides, 1,
        &dep_install_b);
    CHECK(register_plugin(item_present));
    CHECK(register_plugin(character));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.item", FeatureState::kEnabled));
    CHECK(feature_state_is("core.character", FeatureState::kEnabled));
    CHECK(!feature_degraded("core.item"));
    CHECK(std::strcmp(g_dep_order, "BA") == 0);
}

void test_core_character_optional_item_state_degradation_and_topology() {
    reset_all();
    PluginDescriptor character_missing = make_dep_descriptor(
        "core.character", nullptr, 0, g_item_state_optional, 1, nullptr, 0, &dep_install_a);
    CHECK(register_plugin(character_missing));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.character", FeatureState::kEnabled));
    CHECK(feature_degraded("core.character"));

    reset_all();
    const PluginDescriptor character = make_dep_descriptor(
        "core.character", nullptr, 0, g_item_state_optional, 1,
        g_character_state_provides, 1, &dep_install_a);
    const PluginDescriptor item = make_dep_descriptor(
        "core.item", nullptr, 0, nullptr, 0, g_item_state_provides, 1, &dep_install_b);
    CHECK(register_plugin(character));
    CHECK(register_plugin(item));
    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("core.item", FeatureState::kEnabled));
    CHECK(feature_state_is("core.character", FeatureState::kEnabled));
    CHECK(!feature_degraded("core.character"));
    CHECK(std::strcmp(g_dep_order, "BA") == 0);
}

// ③ required 环负例：仍硬失败（kFailed + dependency cycle），不安装。
void test_required_cycle_still_fails() {
    reset_all();
    const PluginDescriptor x = make_dep_descriptor("cycle_x", g_y_api_requires, 1, nullptr, 0,
                                                   g_x_api_provides, 1, &dep_install_a);
    const PluginDescriptor y = make_dep_descriptor("cycle_y", g_x_api_requires, 1, nullptr, 0,
                                                   g_y_api_provides, 1, &dep_install_b);
    CHECK(register_plugin(x));
    CHECK(register_plugin(y));

    CHECK(install_all(make_deps()) == 2);
    CHECK(feature_error_is("cycle_x", "dependency cycle: cycle_x"));
    CHECK(feature_error_is("cycle_y", "dependency cycle: cycle_y"));
    CHECK(g_probe.install_calls == 0);
    CHECK(std::strcmp(g_dep_order, "") == 0);
}

// ④ optional provider 真缺失：无 provider → 段 1 立即 degraded（文案不变）。
void test_optional_missing_provider_degrades() {
    reset_all();
    const PluginDescriptor a = make_dep_descriptor("solo", nullptr, 0, g_eb_api_optional, 1,
                                                   nullptr, 0, &dep_install_a);
    CHECK(register_plugin(a));

    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("solo", FeatureState::kEnabled));
    CHECK(feature_degraded("solo"));
    CHECK(feature_error_is("solo", "missing optional capability: eb.api"));
}

// ⑤ provider 装失败漏报防护：段 1 暂缓 → 段 2 复查失败 → degraded。
void test_optional_pending_degrade_after_provider_failure() {
    reset_all();
    const PluginDescriptor a = make_dep_descriptor("need_b", nullptr, 0, g_b_api_optional, 1,
                                                   nullptr, 0, &dep_install_a);
    const PluginDescriptor b = make_dep_descriptor("provide_b", nullptr, 0, nullptr, 0,
                                                   g_b_api_provides, 1, &dep_install_fail);
    CHECK(register_plugin(a));
    CHECK(register_plugin(b));

    CHECK(install_all(make_deps()) == 1);  // provider 装失败
    CHECK(feature_error_is("provide_b", "install failed"));
    CHECK(feature_state_is("need_b", FeatureState::kEnabled));
    // R2-3：段 2 复查在 capability enable 轮（sync_gates）之后，故 install_all 后尚未补报。
    CHECK(!feature_degraded("need_b"));
    (void)sync_gates();
    CHECK(feature_degraded("need_b"));
    CHECK(feature_error_is("need_b", "missing optional capability: b.api"));
}

// ⑥ 无环 optional 顺序保持：阶段 1 成功，provider 先于 consumer 安装。
void test_optional_acyclic_order_preserved() {
    reset_all();
    const PluginDescriptor consumer = make_dep_descriptor("consumer", nullptr, 0, g_b_api_optional,
                                                          1, nullptr, 0, &dep_install_a);
    const PluginDescriptor provider = make_dep_descriptor("provider", nullptr, 0, nullptr, 0,
                                                          g_b_api_provides, 1, &dep_install_b);
    CHECK(register_plugin(consumer));
    CHECK(register_plugin(provider));

    CHECK(install_all(make_deps()) == 0);
    CHECK(feature_state_is("consumer", FeatureState::kEnabled));
    CHECK(feature_state_is("provider", FeatureState::kEnabled));
    CHECK(!feature_degraded("consumer"));
    CHECK(std::strcmp(g_dep_order, "BA") == 0);
}

// R2-3：段 2 复查时机——provider 在 enable 轮注册 capability 后，consumer 不得误 degraded。
void test_optional_degrade_timing_after_enable() {
    reset_all();
    ResourceClaim provider_gate[] = {claim("hook:test:prov:gate", "prov_provider", "test.prov.gate")};
    const PluginDescriptor consumer = make_dep_descriptor("consumer", nullptr, 0, g_b_api_optional,
                                                          1, nullptr, 0, &dep_install_a);
    const PluginDescriptor provider =
        make_dep_descriptor("provider", nullptr, 0, nullptr, 0, g_b_api_provides, 1,
                            &dep_install_b, provider_gate, 1);
    CHECK(register_plugin(consumer));
    CHECK(register_plugin(provider));

    CHECK(install_all(make_deps()) == 0);
    // provider 门禁关 → installed_disabled、capability 尚未注册；consumer 暂缓但不 degraded。
    CHECK(feature_state_is("provider", FeatureState::kInstalledDisabled));
    CHECK(feature_state_is("consumer", FeatureState::kEnabled));
    CHECK(!feature_degraded("consumer"));

    // R4-②：provider capability 在第 N 帧（N>2）注册前，逐帧复查均静默顺延（不补报/不降级）。
    for (int round = 0; round < 4; ++round) {
        CHECK(qol::host::sync_gates() == 0);
        CHECK(feature_state_is("provider", FeatureState::kInstalledDisabled));
        CHECK(feature_state_is("consumer", FeatureState::kEnabled));
        CHECK(!feature_degraded("consumer"));
        const char* consumer_error = nullptr;
        CHECK(feature_of("consumer", nullptr, nullptr, &consumer_error));
        CHECK(consumer_error == nullptr);
    }

    // enable 轮：门禁打开 → provider 注册 capability → consumer 复查通过，仍不 degraded。
    qol::infra::config_set_bool("test.prov.gate", true);
    (void)sync_gates();
    CHECK(feature_state_is("provider", FeatureState::kEnabled));
    CHECK(feature_state_is("consumer", FeatureState::kEnabled));
    CHECK(!feature_degraded("consumer"));
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

// craft_ui write port 的私有 owner 收敛到合并后的 custom_recipe；旧 craft_ui 被拒。
void test_craft_ui_write_port_owner_is_custom_recipe() {
    reset_all();
    const qol::contracts::QolPortRequirement craft_ui_requirement{
        qol::contracts::QolPortId::QOL_PORT_CRAFT_UI_WRITE,
        {qol::contracts::kQolPortCraftUiWriteVersion.major,
         qol::contracts::kQolPortCraftUiWriteVersion.minor}};
    PluginDescriptor legacy = make_descriptor("craft_ui", nullptr, 0, nullptr, 0);
    legacy.ports = &craft_ui_requirement;
    legacy.port_count = 1;
    CHECK(register_plugin(legacy));
    CHECK(install_all(make_deps()) == 1);
    {
        const char* error = nullptr;
        CHECK(feature_of("craft_ui", nullptr, nullptr, &error));
        CHECK(error != nullptr && std::strstr(error, "private port") != nullptr);
    }

    reset_all();
    const qol::contracts::QolPortRequirement merged[] = {
        {qol::contracts::QolPortId::QOL_PORT_CRAFT_UI_WRITE,
         {qol::contracts::kQolPortCraftUiWriteVersion.major,
          qol::contracts::kQolPortCraftUiWriteVersion.minor}},
        {qol::contracts::QolPortId::QOL_PORT_CUSTOM_RECIPE_WRITE,
         {qol::contracts::kQolPortCustomRecipeWriteVersion.major,
          qol::contracts::kQolPortCustomRecipeWriteVersion.minor}},
    };
    PluginDescriptor merged_owner = make_descriptor("custom_recipe", nullptr, 0, nullptr, 0);
    merged_owner.ports = merged;
    merged_owner.port_count = 2;
    CHECK(register_plugin(merged_owner));
    CHECK(install_all(make_deps()) == 0);
}

}  // namespace

int main() {
    test_success_path();
    test_choice_port_version_gate();
    test_item_port_v33_rejects_legacy_adapters();
    test_save_port_version_and_private_gate();
    test_craft_port_version_and_private_gate();
    test_character_http_port_is_owner_private();
    test_ui_state_read_port_is_owner_private();
    test_move_merge_port_is_owner_private();
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
    test_ptr_slot_dispatch_live_and_backup_handover();
    test_ptr_slot_rollback_releases_slot();
    test_ptr_slot_value_mismatch_fail_closed();
    test_resident_fail_soft_retry();
    test_resident_success_not_reinstalled();
    test_resident_entry_hook_fail_soft_retry();
    test_resident_retry_is_rolled_back_with_later_failure();
    test_atomic_first_member_failure_is_degraded_and_pending();
    test_atomic_middle_member_failure_rolls_back_prefix_and_stays_free();
    test_atomic_failure_releases_members_live_from_previous_round();
    test_atomic_retry_success_is_enabled_once_and_clears_degraded();
    test_atomic_partial_install_publishes_no_capability_or_facade_provider();
    test_atomic_gate_closed_does_not_call_on_enabled();
    test_required_entries_preflight_before_plugin_install();
    test_preflight_failure_isolated_from_independent_plugins();
    test_same_plugin_entry_aliases_fail_before_install_or_hook();
    test_cross_plugin_entry_alias_rejected_but_first_owner_stays_live();
    test_legacy_entry_symbol_id_installs();
    test_elf_export_prefix_rejected_for_other_mechanisms();
    test_optional_cycle_soft_edge();
    test_optional_required_mixed_cycle_real_topology();
    test_core_item_optional_character_state_degradation();
    test_core_character_optional_item_state_degradation_and_topology();
    test_required_cycle_still_fails();
    test_optional_missing_provider_degrades();
    test_optional_pending_degrade_after_provider_failure();
    test_optional_acyclic_order_preserved();
    test_optional_degrade_timing_after_enable();
    test_feature_mode_core_hook_failure_rolls_back();
    test_feature_mode_soft_middle_keeps_degraded_reason();
    test_feature_mode_soft_last_keeps_degraded_reason();
    test_feature_mode_soft_then_hard_failure();
    test_feature_mode_ready_callback_soft_failure_degrades();
    test_feature_mode_ready_callback_hard_failure_fails();
    test_feature_mode_required_ready_failure_aborts_inflight_install();
    test_feature_mode_soft_ready_revoke_then_hard_failure();
    test_feature_mode_ownership_last_soft_failure();
    test_feature_mode_release_waits_for_in_flight();
    test_craft_ui_write_port_owner_is_custom_recipe();
    std::printf("plugin_registry: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
