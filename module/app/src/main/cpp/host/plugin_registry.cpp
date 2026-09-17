#include "host/plugin_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/native/qol_log.h"
#include "contracts/commands.h"
#include "contracts/ownership.h"
#include "game/access/patches.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"
#include "infra/capabilities.h"
#include "infra/config.h"
#include "infra/status.h"
#include "patch/instr_patch.h"

// contracts/plugin.h 在 P2 只前向声明 PluginContext（空载体，宿主服务端口待 P3 冻结）。
// 安装器需要实体才能调用按引用传参的插件回调，故在此补全定义；P3 端口冻结后移回 contracts/。
namespace qol::contracts {
struct PluginContext {};
}  // namespace qol::contracts

namespace qol::host {
namespace {

constexpr uint32_t kMaxPlugins = 32;
constexpr uint32_t kMaxResourcesPerPlugin = 32;
constexpr uint32_t kMaxErrorText = 96;

struct PluginEntry {
    contracts::PluginDescriptor descriptor{};
};

PluginEntry g_plugins[kMaxPlugins];
uint32_t g_plugin_count = 0;

// 装配依赖：install_all 时存入本 TU 静态，供运行期 lifecycle_step 的受限回调失败撤回
// （以及 release 复用同一 hook/unhook 原语）；assembly.cpp 已在装配期传入。
HostDeps g_install_deps{};

// 失败文本静态存储：infra/status 只保存指针，生命周期须 ≥ 进程。
char g_error_text[kMaxPlugins][kMaxErrorText];
// 门禁应用失败（fail-soft）的 degraded 记录：仅记录由本路径置位的 degraded，避免清除
// optional_requires 级联设置的 degraded；error 文本同为静态存储。
char g_gate_error_text[kMaxPlugins][kMaxErrorText];
bool g_gate_degraded[kMaxPlugins];

bool descriptor_valid(const contracts::PluginDescriptor& plugin) {
    if (plugin.id == nullptr || plugin.id[0] == '\0') return false;
    if (plugin.version == nullptr) return false;
    if (plugin.install == nullptr) return false;
    if (plugin.resources_count > kMaxResourcesPerPlugin) return false;
    if (plugin.resources_count > 0 && plugin.resources == nullptr) return false;
    if (plugin.requires_count > 0 && plugin.requires == nullptr) return false;
    if (plugin.optional_count > 0 && plugin.optional_requires == nullptr) return false;
    if (plugin.provides_count > 0 && plugin.provides == nullptr) return false;
    if (plugin.jni_count > 0 && plugin.jni == nullptr) return false;
    return true;
}

// requires 缺失判定；返回 nullptr 表示全部满足，否则为静态错误文本。
const char* check_requires(const contracts::PluginDescriptor& plugin) {
    for (uint32_t i = 0; i < plugin.requires_count; ++i) {
        const contracts::CapabilityRequirement& req = plugin.requires[i];
        if (infra::require_capability(req.id, req.range) == nullptr) {
            return "missing required capability";
        }
    }
    return nullptr;
}

// 非入口机制的同步安装（kCallPatch / kOwnership）；成功后登记运行期地址。
// 返回 nullptr 成功，否则静态错误文本。
const char* install_non_entry_claim(const contracts::ResourceClaim& claim, uintptr_t* out_target) {
    // kOwnership：claim.handler 指向 OwnershipOps，不经 patch 站点。
    if (claim.mechanism == contracts::ResourceMechanism::kOwnership) {
        const contracts::OwnershipOps* ops = contracts::ownership_ops(claim.handler);
        if (ops == nullptr || ops->acquire == nullptr) return "ownership ops missing";
        if (contracts::ownership_acquire(ops, claim.id) != 0) return "ownership acquire rejected";
        if (out_target != nullptr) {
            *out_target = 0;
        }
        return nullptr;
    }
    qol::game::PatchSite site{};
    if (!qol::game::patch_site(claim.symbol, &site)) {
        return "symbol unresolved";
    }
    const qol::patch::Outcome outcome = qol::patch::install_claim(claim, site.addr);
    if (outcome.ok != 0) {
        return outcome.error != nullptr ? outcome.error : "install failed";
    }
    hooks::runtime_set(claim.id, site.addr, nullptr);
    if (out_target != nullptr) {
        *out_target = site.addr;
    }
    return nullptr;
}

// 非入口机制的物理还原：kOwnership 调 ops->release（幂等）；kCallPatch 站点取自运行期记录。
bool release_non_entry_claim(const contracts::ResourceClaim& claim) {
    if (claim.mechanism == contracts::ResourceMechanism::kOwnership) {
        contracts::ownership_release(contracts::ownership_ops(claim.handler), claim.id);
        return true;
    }
    hooks::ResourceRuntime runtime{};
    const uintptr_t target = hooks::runtime_of(claim.id, &runtime) ? runtime.resolved_entry : 0;
    const qol::patch::Outcome outcome = qol::patch::release_claim(claim, target);
    return outcome.ok == 0;
}

// 逆序回滚本插件已安装资源：
// resumable==false 或释放失败 → kTombstone，成功 → kFree（§4.2 物理归还）。
void rollback_resources(const contracts::ResourceClaim* const* claims,
                        const uintptr_t* entries, uint32_t count,
                        const HostDeps& deps) {
    for (uint32_t i = count; i > 0; --i) {
        const contracts::ResourceClaim& claim = *claims[i - 1];
        bool released = false;
        if (claim.resumable) {
            if (claim.mechanism == contracts::ResourceMechanism::kEntryHook) {
                released = deps.unhook_fn != nullptr &&
                           deps.unhook_fn(reinterpret_cast<void*>(entries[i - 1])) == 0;
            } else {
                released = release_non_entry_claim(claim);
            }
        }
        if (released) {
            hooks::runtime_clear(claim.id);
        }
        hooks::set_state(claim.id, released ? contracts::ResourceState::kFree
                                            : contracts::ResourceState::kTombstone);
    }
}

// 撤回单条已安装资源（§3.3 受限回调失败路径）：成功 → kFree + runtime 清除；
// 失败 → kTombstone + 隔离（保留占用，禁止重试直到进程重启）。
void revoke_resource(const char* resource_id, const HostDeps& deps) {
    const contracts::ResourceClaim* claim = hooks::find_claim(resource_id);
    if (claim == nullptr) return;
    bool released = false;
    if (claim->mechanism == contracts::ResourceMechanism::kEntryHook) {
        hooks::ResourceRuntime runtime{};
        const uintptr_t entry =
            hooks::runtime_of(resource_id, &runtime) ? runtime.resolved_entry : 0;
        released = hooks::release_claim_entry_hook(*claim, entry, deps.unhook_fn).ok == 0;
    } else {
        released = claim->resumable && release_non_entry_claim(*claim);
    }
    if (released) {
        hooks::set_state(resource_id, contracts::ResourceState::kFree);
        hooks::runtime_clear(resource_id);
    } else {
        hooks::set_state(resource_id, contracts::ResourceState::kTombstone);
        hooks::isolated_set(resource_id, true);
    }
}

// 逆序撤回一组已记录资源（§3.2 语义 5：禁止半装对外可见）。
void rollback_installed(const contracts::ResourceClaim* const* claims, uint32_t count,
                        const HostDeps& deps) {
    for (uint32_t i = count; i > 0; --i) {
        revoke_resource(claims[i - 1]->id, deps);
    }
}

// 逆序撤回插件当前处于已装态（live/disabled）的全部资源；用于还原事务启动失败时的同步兜底。
void revoke_installed_resources(const contracts::PluginDescriptor& plugin, const HostDeps& deps) {
    for (uint32_t i = plugin.resources_count; i > 0; --i) {
        const contracts::ResourceClaim& claim = plugin.resources[i - 1];
        const contracts::ResourceState state = hooks::state_of(claim.id);
        if (state == contracts::ResourceState::kLive ||
            state == contracts::ResourceState::kDisabled) {
            revoke_resource(claim.id, deps);
        }
    }
}

// 门禁监督辅助：同步某插件带 gate 资源的登记状态，并汇总目标态。
// 入口 hook 只改写 disabled/live 两态：free（未安装）与 tombstone（保留占用）不得被监督改写。
// 非入口机制（call patch / ownership）不支持空转（§4.2 规则 6）：门禁变化直接物理安装/还原。
// 安装失败按 §8 逐资源策略处置（与 install_all 一致）：
//   · kRequired×kFailClosed → tombstone + isolated + mechanism_failed（不可重试，插件 failed）；
//   · 其余（kOptional×任一 / kRequired×kFailSoft）→ 保持 free 供下一逻辑帧重试，
//     经 out_degraded/out_error_* 记录原因（degraded，不改生命周期态）。
// 释放/还原失败仍是不可重试形态 → tombstone + isolated（§4.2）。
// 同步执行原因：单一指令字改写为同步 fail-closed 原语，且没有在途 wrapper 需等待；
// 生命周期安装/还原仍走事务引擎（lifecycle_apply/lifecycle_step）。
void sync_plugin_resources(const contracts::PluginDescriptor& plugin, bool* out_has_gate,
                           bool* out_all_want, bool* out_mechanism_failed, bool* out_degraded,
                           const char** out_error_claim, const char** out_error_reason) {
    bool has_gate = false;
    bool all_want = true;
    bool mechanism_failed = false;
    bool degraded = false;
    const char* error_claim = nullptr;
    const char* error_reason = nullptr;
    // 本次门禁同步新装成功的非入口资源；失败时逆序还原，禁止半装对外可见。
    const contracts::ResourceClaim* installed[kMaxResourcesPerPlugin];
    uint32_t installed_count = 0;
    for (uint32_t r = 0; r < plugin.resources_count; ++r) {
        const contracts::ResourceClaim& claim = plugin.resources[r];
        if (claim.gate == nullptr) continue;
        has_gate = true;
        const bool want = infra::config_get_bool(claim.gate, false);
        if (!want) all_want = false;
        const contracts::ResourceState now = hooks::state_of(claim.id);
        if (claim.mechanism == contracts::ResourceMechanism::kEntryHook) {
            if (want && now == contracts::ResourceState::kDisabled) {
                hooks::set_state(claim.id, contracts::ResourceState::kLive);
            } else if (!want && now == contracts::ResourceState::kLive) {
                hooks::set_state(claim.id, contracts::ResourceState::kDisabled);
            }
            continue;
        }
        if (want && now == contracts::ResourceState::kFree) {
            uintptr_t target = 0;
            const char* error = install_non_entry_claim(claim, &target);
            if (error != nullptr) {
                const bool fail_closed_required =
                    claim.requirement == contracts::ResourceRequirement::kRequired &&
                    claim.failure == contracts::ResourceFailure::kFailClosed;
                if (fail_closed_required) {
                    hooks::set_state(claim.id, contracts::ResourceState::kTombstone);
                    hooks::isolated_set(claim.id, true);
                    mechanism_failed = true;
                } else {
                    // fail-soft：保持 free，门禁每逻辑帧重试；只记 degraded + 原因。
                    hooks::set_state(claim.id, contracts::ResourceState::kFree);
                    degraded = true;
                    error_claim = claim.id;
                    error_reason = error;
                }
            } else {
                hooks::set_state(claim.id, contracts::ResourceState::kLive);
                installed[installed_count++] = &claim;
            }
        } else if (!want && now == contracts::ResourceState::kLive) {
            if (release_non_entry_claim(claim)) {
                hooks::set_state(claim.id, contracts::ResourceState::kFree);
                hooks::runtime_clear(claim.id);
            } else {
                hooks::set_state(claim.id, contracts::ResourceState::kTombstone);
                hooks::isolated_set(claim.id, true);
                mechanism_failed = true;
            }
        }
    }
    if (mechanism_failed) {
        for (uint32_t i = installed_count; i > 0; --i) {
            const contracts::ResourceClaim& claim = *installed[i - 1];
            if (release_non_entry_claim(claim)) {
                hooks::set_state(claim.id, contracts::ResourceState::kFree);
                hooks::runtime_clear(claim.id);
            } else {
                hooks::set_state(claim.id, contracts::ResourceState::kTombstone);
                hooks::isolated_set(claim.id, true);
            }
        }
    }
    *out_has_gate = has_gate;
    *out_all_want = all_want;
    *out_mechanism_failed = mechanism_failed;
    *out_degraded = degraded;
    if (out_error_claim != nullptr) *out_error_claim = error_claim;
    if (out_error_reason != nullptr) *out_error_reason = error_reason;
}

// ---- 生命周期事务跟踪（主线程唯一执行器；每插件至多一个在途事务）----

struct LifecycleTx {
    bool in_use = false;
    bool ready_failed = false;  // 受限回调失败已置 failed：终态不再覆盖
    uint64_t tx_id = 0;
    uint32_t plugin_index = 0;
    hooks::TxKind kind = hooks::TxKind::kInstall;
};

// 槽位与插件同容量：事务引擎保证每插件至多一个未终态事务。
LifecycleTx g_lifecycle_txs[kMaxPlugins];

uint32_t plugin_index_of(const char* plugin_id) {
    if (plugin_id == nullptr) return kMaxPlugins;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        if (std::strcmp(g_plugins[i].descriptor.id, plugin_id) == 0) return i;
    }
    return kMaxPlugins;
}

bool plugin_has_active_tx(const char* plugin_id) {
    if (plugin_id == nullptr) return false;
    for (const LifecycleTx& slot : g_lifecycle_txs) {
        if (!slot.in_use) continue;
        if (std::strcmp(g_plugins[slot.plugin_index].descriptor.id, plugin_id) == 0) return true;
    }
    return false;
}

bool plugin_isolated(const contracts::PluginDescriptor& plugin) {
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        if (hooks::isolated_get(plugin.resources[i].id)) return true;
    }
    return false;
}

// 全部带 gate 资源是否都开启；无 gate 资源视为开（与 install 路径一致）。
bool plugin_all_gates_want(const contracts::PluginDescriptor& plugin) {
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        const contracts::ResourceClaim& claim = plugin.resources[i];
        if (claim.gate == nullptr) continue;
        if (!infra::config_get_bool(claim.gate, false)) return false;
    }
    return true;
}

bool plugin_has_gate(const contracts::PluginDescriptor& plugin) {
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        if (plugin.resources[i].gate != nullptr) return true;
    }
    return false;
}

// ---- 能力注册与撤销级联（§3.4.1 / §3.4.4）----

enum class CapRegistration : uint8_t { kNone = 0, kRegistered = 1, kRevoking = 2 };
CapRegistration g_cap_registration[kMaxPlugins];
bool g_cascade_visited[kMaxPlugins];

bool plugin_provides_capability(const contracts::PluginDescriptor& plugin) {
    return plugin.provides_count > 0 && plugin.provides != nullptr;
}

// 进入 enabled 时注册（§3.4.4；幂等：已注册跳过）。
void register_provided_capabilities(uint32_t index) {
    const contracts::PluginDescriptor& plugin = g_plugins[index].descriptor;
    if (!plugin_provides_capability(plugin)) return;
    if (g_cap_registration[index] == CapRegistration::kRegistered) return;
    for (uint32_t i = 0; i < plugin.provides_count; ++i) {
        const contracts::CapabilityProvisionRef& ref = plugin.provides[i];
        if (ref.id == nullptr || ref.iface == nullptr) continue;
        // 重复注册（同 id）返回 false，视为已注册；容量满/非法同样只记录、不阻断。
        infra::register_capability(infra::CapabilityProvision{ref.id, ref.version, ref.iface});
    }
    g_cap_registration[index] = CapRegistration::kRegistered;
}

void unregister_provided_capabilities(uint32_t index) {
    const contracts::PluginDescriptor& plugin = g_plugins[index].descriptor;
    if (!plugin_provides_capability(plugin)) return;
    for (uint32_t i = 0; i < plugin.provides_count; ++i) {
        if (plugin.provides[i].id != nullptr) {
            infra::unregister_capability(plugin.provides[i].id);
        }
    }
    g_cap_registration[index] = CapRegistration::kNone;
}

void cascade_dependents(const char* capability_id);

// 级联停用 required 消费者（§3.4.1 第 3 条）：关消费者门禁 → 释放资源 →
// 递归撤销其自身提供的能力 → 置 installed_disabled + reason=cascade_stopped。
void cascade_stop_plugin(uint32_t index) {
    if (index >= g_plugin_count || g_cascade_visited[index]) return;
    g_cascade_visited[index] = true;
    const contracts::PluginDescriptor& plugin = g_plugins[index].descriptor;
    if (plugin.on_disabled != nullptr) {
        contracts::PluginContext ctx{};
        plugin.on_disabled(ctx);
    }
    // 释放消费者资源：走同步回滚路径（§3.4.1 允许事务或同步回滚）。
    for (uint32_t r = plugin.resources_count; r > 0; --r) {
        const contracts::ResourceClaim& claim = plugin.resources[r - 1];
        const contracts::ResourceState state = hooks::state_of(claim.id);
        if (state == contracts::ResourceState::kLive ||
            state == contracts::ResourceState::kDisabled) {
            revoke_resource(claim.id, g_install_deps);
        }
    }
    // 消费者自身提供的能力：注销并继续级联（递归即逆拓扑序）。
    if (g_cap_registration[index] == CapRegistration::kRegistered) {
        g_cap_registration[index] = CapRegistration::kRevoking;
        for (uint32_t i = 0; i < plugin.provides_count; ++i) {
            cascade_dependents(plugin.provides[i].id);
        }
        unregister_provided_capabilities(index);
    }
    infra::status_update(plugin.id, infra::FeatureState::kInstalledDisabled, false,
                         "cascade_stopped");
}

// 依赖失效级联：requires 命中 → 逆拓扑停用；optional_requires 命中 → degraded 标记。
void cascade_dependents(const char* capability_id) {
    if (capability_id == nullptr) return;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        const contracts::PluginDescriptor& plugin = g_plugins[i].descriptor;
        bool required = false;
        for (uint32_t r = 0; r < plugin.requires_count; ++r) {
            if (plugin.requires[r].id != nullptr &&
                std::strcmp(plugin.requires[r].id, capability_id) == 0) {
                required = true;
                break;
            }
        }
        if (required) {
            infra::FeatureStatus st{};
            if (infra::status_get(plugin.id, &st) &&
                (st.state == infra::FeatureState::kEnabled ||
                 st.state == infra::FeatureState::kInstalledDisabled)) {
                cascade_stop_plugin(i);
            }
            continue;
        }
        for (uint32_t o = 0; o < plugin.optional_count; ++o) {
            if (plugin.optional_requires[o].id != nullptr &&
                std::strcmp(plugin.optional_requires[o].id, capability_id) == 0) {
                infra::status_set_degraded(plugin.id, true);
            }
        }
    }
}

// 提供者撤销（§3.4.4）：关闭提供者门禁（撤销期拒绝新调用）→ 级联 → 注销。
void begin_revoke_provider(uint32_t index) {
    const contracts::PluginDescriptor& plugin = g_plugins[index].descriptor;
    if (!plugin_provides_capability(plugin)) return;
    if (plugin.on_disabled != nullptr) {
        contracts::PluginContext ctx{};
        plugin.on_disabled(ctx);
    }
    if (g_cap_registration[index] != CapRegistration::kRegistered) return;
    g_cap_registration[index] = CapRegistration::kRevoking;
    for (uint32_t i = 0; i < plugin.provides_count; ++i) {
        cascade_dependents(plugin.provides[i].id);
    }
    unregister_provided_capabilities(index);
}

}  // namespace

bool register_plugin(const contracts::PluginDescriptor& plugin) {
    if (!descriptor_valid(plugin)) return false;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        if (std::strcmp(g_plugins[i].descriptor.id, plugin.id) == 0) return false;
    }
    if (g_plugin_count >= kMaxPlugins) return false;

    // 资源登记在声明期完成（一资源一安装者）；登记失败即视为描述符非法。
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        if (!hooks::register_claim(plugin.resources[i])) return false;
    }

    g_plugins[g_plugin_count].descriptor = plugin;
    ++g_plugin_count;
    return true;
}

int install_all(const HostDeps& deps) {
    g_install_deps = deps;
    int failed_plugins = 0;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        const contracts::PluginDescriptor& plugin = g_plugins[i].descriptor;
        infra::status_register(plugin.id, plugin.version);
        infra::status_update(plugin.id, infra::FeatureState::kResolving, false, nullptr);

        // 1) requires 校验：缺必需能力 → blocked（P2 无插件声明 requires）。
        const char* blocked_error = check_requires(plugin);
        if (blocked_error != nullptr) {
            infra::status_update(plugin.id, infra::FeatureState::kBlocked, false, blocked_error);
            ++failed_plugins;
            continue;
        }

        // 2) 插件自身初始化。
        contracts::PluginContext ctx{};
        if (plugin.install(ctx) != 0) {
            infra::status_update(plugin.id, infra::FeatureState::kFailed, false, "install failed");
            ++failed_plugins;
            continue;
        }

        // 3) 资源逐条声明式安装；失败按 §8 逐资源策略处置：
        //    kRequired×kFailClosed → 逆序回滚 + failed；其余（kRequired×kFailSoft /
        //    kOptional×任一）→ 记录 + degraded，跳过该资源继续安装其余。
        const contracts::ResourceClaim* installed_claims[kMaxResourcesPerPlugin];
        uintptr_t installed_entries[kMaxResourcesPerPlugin];
        uint32_t installed_count = 0;
        bool all_gates_on = true;
        bool resource_failed = false;
        bool plugin_aborted = false;  // 受限回调失败已置 failed，跳过统一回滚分支
        bool degraded = false;
        const char* failed_claim_id = nullptr;
        const char* resource_error = nullptr;
        const char* degraded_claim_id = nullptr;
        const char* degraded_error = nullptr;

        // 返回 true = 该资源失败必须整体回滚（required + fail-closed）。
        auto handle_claim_failure = [&](const contracts::ResourceClaim& claim,
                                        const char* error) -> bool {
            const bool fail_closed_required =
                claim.requirement == contracts::ResourceRequirement::kRequired &&
                claim.failure == contracts::ResourceFailure::kFailClosed;
            if (fail_closed_required) {
                resource_failed = true;
                failed_claim_id = claim.id;
                resource_error = error;
                return true;
            }
            degraded = true;
            degraded_claim_id = claim.id;
            degraded_error = error;
            return false;
        };

        for (uint32_t r = 0; r < plugin.resources_count; ++r) {
            const contracts::ResourceClaim& claim = plugin.resources[r];
            const bool gate_on =
                claim.gate != nullptr ? infra::config_get_bool(claim.gate, false) : true;
            if (claim.gate != nullptr && !gate_on) all_gates_on = false;

            // 非入口机制（call patch / ownership）不支持空转（§4.2 规则 6）：门禁关不安装、登记保持 free。
            if (claim.mechanism != contracts::ResourceMechanism::kEntryHook) {
                if (claim.gate != nullptr && !gate_on) {
                    continue;
                }
                uintptr_t target = 0;
                const char* error = install_non_entry_claim(claim, &target);
                if (error != nullptr) {
                    if (handle_claim_failure(claim, error)) break;
                    continue;
                }
                // 安装成功登记 live（与入口 hook 路径一致；sync_gates 路径随后会再写同值）。
                hooks::set_state(claim.id, contracts::ResourceState::kLive);
                installed_claims[installed_count] = &claim;
                installed_entries[installed_count] = target;
                ++installed_count;
                continue;
            }

            bool followed = false;
            const uintptr_t entry =
                deps.resolve_entry != nullptr ? deps.resolve_entry(claim.symbol, &followed) : 0;
            if (entry == 0) {
                if (handle_claim_failure(claim, "symbol unresolved")) break;
                continue;
            }

            const hooks::InstallOutcome outcome =
                hooks::install_claim_entry_hook(claim, entry, deps.hook_fn, gate_on);
            if (outcome.ok != 0) {
                if (handle_claim_failure(
                        claim, outcome.error != nullptr ? outcome.error : "install failed")) {
                    break;
                }
                continue;
            }
            // 同步路径同样登记运行期入口：运行期 deactivate/撤回的物理归还要用 resolved_entry。
            hooks::runtime_set(claim.id, entry, outcome.backup);
            // 立即把原函数入口交还所有者（§3.3「backup 交还」）。返回非 0 = 受限回调失败：
            // 按逆序同步撤回本插件全部已装资源（含本条），禁止半装对外可见（§3.2 语义 5）。
            if (plugin.on_resource_ready != nullptr &&
                plugin.on_resource_ready(ctx, claim.id, outcome.backup) != 0) {
                installed_claims[installed_count] = &claim;
                installed_entries[installed_count] = entry;
                ++installed_count;
                rollback_installed(installed_claims, installed_count, deps);
                infra::status_update(plugin.id, infra::FeatureState::kFailed, false,
                                     "resource_ready_failed");
                QOL_LOG_WARN(QolDomain::kCore, "plugin %s resource_ready_failed on %s", plugin.id,
                             claim.id);
                ++failed_plugins;
                plugin_aborted = true;
                break;
            }
            installed_claims[installed_count] = &claim;
            installed_entries[installed_count] = entry;
            ++installed_count;
        }

        if (plugin_aborted) {
            continue;
        }

        if (resource_failed) {
            rollback_resources(installed_claims, installed_entries, installed_count, deps);
            std::snprintf(g_error_text[i], sizeof(g_error_text[i]), "resource %s: %s",
                          failed_claim_id != nullptr ? failed_claim_id : "?",
                          resource_error != nullptr ? resource_error : "failed");
            infra::status_update(plugin.id, infra::FeatureState::kFailed, false, g_error_text[i]);
            QOL_LOG_WARN(QolDomain::kCore, "plugin %s install failed: %s", plugin.id,
                         g_error_text[i]);
            ++failed_plugins;
            continue;
        }

        // 4) 全部成功（含 fail-soft 跳过）：带 gate 的资源全开 → enabled；否则 installed_disabled。
        //    fail-soft 失败只置 degraded 标记 + error 文案，不改生命周期态（§8）。
        const bool enabled = all_gates_on;
        const char* status_error = nullptr;
        if (degraded) {
            std::snprintf(g_error_text[i], sizeof(g_error_text[i]), "resource %s: %s",
                          degraded_claim_id != nullptr ? degraded_claim_id : "?",
                          degraded_error != nullptr ? degraded_error : "failed");
            status_error = g_error_text[i];
            infra::status_set_degraded(plugin.id, true);
            // 记录由本路径置位，供 sync_gates 在资源重试成功后清除标记。
            g_gate_degraded[i] = true;
            QOL_LOG_WARN(QolDomain::kCore, "plugin %s degraded on %s: %s", plugin.id,
                         degraded_claim_id != nullptr ? degraded_claim_id : "?", status_error);
        }
        infra::status_update(
            plugin.id,
            enabled ? infra::FeatureState::kEnabled : infra::FeatureState::kInstalledDisabled,
            enabled, status_error);
        if (enabled) {
            if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
            // §3.4.4：进入 enabled 时注册能力（提供者门禁已由 on_enabled 打开）。
            register_provided_capabilities(i);
        } else {
            if (plugin.on_disabled != nullptr) plugin.on_disabled(ctx);
        }
    }
    return failed_plugins;
}

uint32_t sync_gates() {
    uint32_t changed = 0;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        const contracts::PluginDescriptor& plugin = g_plugins[i].descriptor;
        infra::FeatureStatus status{};
        if (!infra::status_get(plugin.id, &status)) continue;
        // 只监督已按门禁落态的插件；failed/blocked/invalid 等不参与（避免覆盖失败态）。
        if (status.state != infra::FeatureState::kEnabled &&
            status.state != infra::FeatureState::kInstalledDisabled) {
            continue;
        }
        // 隔离或在途事务期间不参与门禁监督，避免与事务中间态冲突。
        if (plugin_isolated(plugin)) continue;
        if (plugin_has_active_tx(plugin.id)) continue;
        // 级联停用不自动恢复（§3.4.1 第 6 条）：保留显式 enable 路径。
        if (status.error != nullptr && std::strcmp(status.error, "cascade_stopped") == 0) {
            continue;
        }
        if (!plugin_has_gate(plugin)) continue;  // 无带 gate 资源 → 不参与门禁监督

        // 撤销先于关资源（§3.4.4）：provider 由 enabled 转 disable 时先关提供者门禁并级联注销。
        if (status.state == infra::FeatureState::kEnabled && !plugin_all_gates_want(plugin) &&
            plugin_provides_capability(plugin)) {
            begin_revoke_provider(i);
        }

        bool has_gate = false;
        bool all_want = true;
        bool mechanism_failed = false;
        bool gate_degraded = false;
        const char* gate_error_claim = nullptr;
        const char* gate_error_reason = nullptr;
        sync_plugin_resources(plugin, &has_gate, &all_want, &mechanism_failed, &gate_degraded,
                              &gate_error_claim, &gate_error_reason);
        if (mechanism_failed) {
            // 机制门禁应用失败（fail-closed 安装失败 / 释放失败）：保留占用并隔离，插件置 failed。
            g_gate_degraded[i] = false;
            infra::status_update(plugin.id, infra::FeatureState::kFailed, false,
                                 "mechanism gate apply failed");
            ++changed;
            continue;
        }
        const bool target_enabled = all_want;
        const infra::FeatureState target_state = target_enabled
            ? infra::FeatureState::kEnabled : infra::FeatureState::kInstalledDisabled;

        // fail-soft 安装失败：资源保持 free 供下一逻辑帧重试；插件 degraded + error，不改生命周期态。
        // 日志只在失败streak首帧输出，避免逐帧刷屏（同因去重）。
        if (gate_degraded) {
            const bool newly_degraded = !g_gate_degraded[i];
            std::snprintf(g_gate_error_text[i], sizeof(g_gate_error_text[i]), "resource %s: %s",
                          gate_error_claim != nullptr ? gate_error_claim : "?",
                          gate_error_reason != nullptr ? gate_error_reason : "failed");
            if (newly_degraded) {
                g_gate_degraded[i] = true;
                infra::status_set_degraded(plugin.id, true);
                QOL_LOG_WARN(QolDomain::kCore, "plugin %s gate install deferred on %s: %s", plugin.id,
                             gate_error_claim != nullptr ? gate_error_claim : "?",
                             g_gate_error_text[i]);
            }
            contracts::PluginContext ctx{};
            if (status.state != target_state) {
                infra::status_update(plugin.id, target_state, target_enabled, g_gate_error_text[i]);
                if (target_enabled) {
                    if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
                    register_provided_capabilities(i);
                } else if (!plugin_provides_capability(plugin)) {
                    if (plugin.on_disabled != nullptr) plugin.on_disabled(ctx);
                }
                ++changed;
            } else {
                infra::status_update(plugin.id, target_state, target_enabled, g_gate_error_text[i]);
            }
            continue;
        }
        // 本轮无门禁失败：若之前由本路径置位 degraded，现资源已就绪 → 清除标记与 error。
        if (g_gate_degraded[i]) {
            g_gate_degraded[i] = false;
            infra::status_set_degraded(plugin.id, false);
            if (status.error != nullptr) {
                infra::status_update(plugin.id, status.state, status.enabled, nullptr);
            }
        }
        if (status.state == target_state) continue;

        contracts::PluginContext ctx{};
        infra::status_update(plugin.id, target_state, target_enabled, nullptr);
        if (target_enabled) {
            if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
            register_provided_capabilities(i);
        } else if (!plugin_provides_capability(plugin)) {
            // provider 的 on_disabled 已由 begin_revoke_provider 调用（避免重复）。
            if (plugin.on_disabled != nullptr) plugin.on_disabled(ctx);
        }
        ++changed;
    }
    return changed;
}

bool lifecycle_apply(const char* plugin_id, int32_t action) {
    const contracts::PluginDescriptor* plugin = find_plugin(plugin_id);
    if (plugin == nullptr) return false;
    // 前置拒绝：同插件在途事务 / 任一资源已隔离（隔离后禁止重试，直到进程重启）。
    if (plugin_has_active_tx(plugin->id)) return false;
    if (plugin_isolated(*plugin)) return false;

    infra::FeatureStatus status{};
    if (!infra::status_get(plugin->id, &status)) return false;

    contracts::PluginContext ctx{};
    switch (action) {
        case contracts::kFeatureLifecycleEnable:
        case contracts::kFeatureLifecycleDisable:
            // 开关语义归配置门禁（/api/config/set），本端点为单一写者：不接受 enable/disable。
            return false;

        case contracts::kFeatureLifecycleDeactivate: {
            if (status.state != infra::FeatureState::kEnabled &&
                status.state != infra::FeatureState::kInstalledDisabled) {
                return false;
            }
            const infra::FeatureState previous = status.state;
            infra::status_update(plugin->id, infra::FeatureState::kDeactivating, false, nullptr);
            const uint64_t tx_id = hooks::tx_begin(plugin->id, hooks::TxKind::kRelease);
            if (tx_id == 0) {
                // 进入 deactivating 失败：回滚为原状态。
                infra::status_update(plugin->id, previous,
                                     previous == infra::FeatureState::kEnabled, nullptr);
                return false;
            }
            const uint32_t index = plugin_index_of(plugin->id);
            // §3.4.4：deactivate 走 disable 撤销路径——关提供者门禁 → 级联 → 注销。
            begin_revoke_provider(index);
            for (LifecycleTx& slot : g_lifecycle_txs) {
                if (slot.in_use) continue;
                slot.in_use = true;
                slot.tx_id = tx_id;
                slot.plugin_index = index;
                slot.kind = hooks::TxKind::kRelease;
                return true;
            }
            return false;  // 槽位与插件同容量，正常不可达
        }

        case contracts::kFeatureLifecycleActivate: {
            // §3.2：inactive → resolving 为幂等重入；failed → resolving 为显式重试
            // （隔离态已在函数前置拒绝中拦截，未隔离的失败可安全重装）。
            if (status.state != infra::FeatureState::kInactive &&
                status.state != infra::FeatureState::kFailed) {
                return false;
            }
            infra::status_update(plugin->id, infra::FeatureState::kResolving, false, nullptr);
            // §3.3：install 只做插件自身初始化（幂等重入）；失败即 failed，不启动事务。
            if (plugin->install != nullptr && plugin->install(ctx) != 0) {
                infra::status_update(plugin->id, infra::FeatureState::kFailed, false,
                                     "plugin install failed");
                return false;
            }
            const uint64_t tx_id = hooks::tx_begin(plugin->id, hooks::TxKind::kInstall);
            if (tx_id == 0) {
                infra::status_update(plugin->id, infra::FeatureState::kInactive, false, nullptr);
                return false;
            }
            const uint32_t index = plugin_index_of(plugin->id);
            for (LifecycleTx& slot : g_lifecycle_txs) {
                if (slot.in_use) continue;
                slot.in_use = true;
                slot.tx_id = tx_id;
                slot.plugin_index = index;
                slot.kind = hooks::TxKind::kInstall;
                return true;
            }
            return false;  // 槽位与插件同容量，正常不可达
        }

        default:
            return false;
    }
}

uint32_t lifecycle_step() {
    uint32_t advanced = 0;
    for (LifecycleTx& slot : g_lifecycle_txs) {
        if (!slot.in_use) continue;
        const contracts::PluginDescriptor& plugin = g_plugins[slot.plugin_index].descriptor;

        hooks::TxStep out{};
        hooks::tx_step(slot.tx_id, &out);
        ++advanced;

        // 安装步成功：宿主把原函数入口交还所有者（§3.3 backup 交还）。
        // backup 非空是「本步真实安装」的判据：幂等跳过步 backup 为 null，交还会把
        // 插件已持有的有效入口覆盖成 null（wrapper 随即 fail-closed），故跳过。
        bool switched_to_release = false;
        if (slot.kind == hooks::TxKind::kInstall && out.resource_id != nullptr &&
            out.backup != nullptr) {
            if (slot.ready_failed) {
                // 已进入回滚：安装事务后续步新装出的资源立即撤回，禁止半装对外可见。
                revoke_resource(out.resource_id, g_install_deps);
            } else if (plugin.on_resource_ready != nullptr) {
                contracts::PluginContext ctx{};
                if (plugin.on_resource_ready(ctx, out.resource_id, out.backup) != 0) {
                    // 受限回调失败：置 failed，并启动还原事务回滚全部已装资源（§3.2 语义 5）。
                    infra::status_update(plugin.id, infra::FeatureState::kFailed, false,
                                         "resource_ready_failed");
                    QOL_LOG_WARN(QolDomain::kCore, "plugin %s resource_ready_failed on %s",
                                 plugin.id, out.resource_id);
                    slot.ready_failed = true;
                    const uint64_t release = hooks::tx_begin(plugin.id, hooks::TxKind::kRelease);
                    if (release != 0) {
                        // 安装事务已终态：改为驱动还原事务。
                        slot.tx_id = release;
                        slot.kind = hooks::TxKind::kRelease;
                        switched_to_release = true;
                    } else {
                        // 安装事务仍在途：退化为逐条同步撤回兜底（后续步新装资源亦即时撤回）。
                        revoke_installed_resources(plugin, g_install_deps);
                    }
                }
            }
        }
        if (switched_to_release) continue;
        if (!out.finished) continue;

        if (slot.ready_failed) {
            // 已因受限回调置 failed：保留 failed 态，不执行终态状态/回调覆盖。
            slot.in_use = false;
            slot.ready_failed = false;
            slot.tx_id = 0;
            continue;
        }

        hooks::TxStatus status{};
        const bool have_status = hooks::tx_query(slot.tx_id, &status);
        const bool succeeded = have_status && status.state == hooks::TxState::kSucceeded;
        contracts::PluginContext ctx{};
        if (succeeded) {
            if (slot.kind == hooks::TxKind::kInstall) {
                const bool enabled = plugin_all_gates_want(plugin);
                infra::status_update(
                    plugin.id,
                    enabled ? infra::FeatureState::kEnabled
                            : infra::FeatureState::kInstalledDisabled,
                    enabled, nullptr);
                if (enabled) {
                    if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
                    // §3.4.4：install 成功后进入 enabled 时注册能力。
                    register_provided_capabilities(slot.plugin_index);
                } else {
                    if (plugin.on_disabled != nullptr) plugin.on_disabled(ctx);
                }
            } else {
                // §3.3：deactivate 只做插件自身清理（幂等）；资源已逆序还原，随后置 inactive。
                if (plugin.deactivate != nullptr) plugin.deactivate(ctx);
                infra::status_update(plugin.id, infra::FeatureState::kInactive, false, nullptr);
            }
        } else {
            // 失败原因取事务记录；两者均无则用固定静态文本。
            const char* error =
                have_status && status.error != nullptr
                    ? status.error
                    : (have_status && status.failed_resource_id != nullptr
                           ? status.failed_resource_id
                           : "lifecycle transaction failed");
            infra::status_update(plugin.id, infra::FeatureState::kFailed, false, error);
            QOL_LOG_WARN(QolDomain::kCore, "plugin %s lifecycle failed: %s", plugin.id, error);
        }
        slot.in_use = false;
        slot.tx_id = 0;
    }
    return advanced;
}

const contracts::PluginDescriptor* find_plugin(const char* id) {
    if (id == nullptr) return nullptr;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        if (std::strcmp(g_plugins[i].descriptor.id, id) == 0) {
            return &g_plugins[i].descriptor;
        }
    }
    return nullptr;
}

uint32_t plugin_count() {
    return g_plugin_count;
}

void plugin_registry_reset_for_test() {
    g_plugin_count = 0;
    g_install_deps = HostDeps{};
    for (LifecycleTx& slot : g_lifecycle_txs) {
        slot = LifecycleTx{};
    }
    for (uint32_t i = 0; i < kMaxPlugins; ++i) {
        g_cap_registration[i] = CapRegistration::kNone;
        g_cascade_visited[i] = false;
        g_gate_degraded[i] = false;
    }
}

}  // namespace qol::host
