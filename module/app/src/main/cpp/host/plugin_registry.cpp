#include "host/plugin_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/native/qol_log.h"
#include "contracts/commands.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"
#include "infra/capabilities.h"
#include "infra/config.h"
#include "infra/status.h"

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

// 失败文本静态存储：infra/status 只保存指针，生命周期须 ≥ 进程。
char g_error_text[kMaxPlugins][kMaxErrorText];

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

// 逆序回滚本插件已安装资源：
// resumable==false 或 unhook 缺失/失败 → kTombstone，成功 → kFree（§4.2 物理归还）。
void rollback_resources(const contracts::ResourceClaim* const* claims,
                        const uintptr_t* entries, uint32_t count,
                        const HostDeps& deps) {
    for (uint32_t i = count; i > 0; --i) {
        const contracts::ResourceClaim& claim = *claims[i - 1];
        int rc = -1;
        if (claim.resumable && deps.unhook_fn != nullptr) {
            rc = deps.unhook_fn(reinterpret_cast<void*>(entries[i - 1]));
        }
        if (rc == 0) {
            hooks::runtime_clear(claim.id);
        }
        hooks::set_state(claim.id, rc == 0 ? contracts::ResourceState::kFree
                                           : contracts::ResourceState::kTombstone);
    }
}

// 门禁监督辅助：同步某插件带 gate 资源的登记状态，并汇总目标态。
// 只改写 disabled/live 两态：free（未安装）与 tombstone（保留占用）不得被监督改写。
void sync_plugin_resources(const contracts::PluginDescriptor& plugin, bool* out_has_gate,
                           bool* out_all_want) {
    bool has_gate = false;
    bool all_want = true;
    for (uint32_t r = 0; r < plugin.resources_count; ++r) {
        const contracts::ResourceClaim& claim = plugin.resources[r];
        if (claim.gate == nullptr) continue;
        has_gate = true;
        const bool want = infra::config_get_bool(claim.gate, false);
        const contracts::ResourceState now = hooks::state_of(claim.id);
        if (want && now == contracts::ResourceState::kDisabled) {
            hooks::set_state(claim.id, contracts::ResourceState::kLive);
        } else if (!want && now == contracts::ResourceState::kLive) {
            hooks::set_state(claim.id, contracts::ResourceState::kDisabled);
        }
        if (!want) all_want = false;
    }
    *out_has_gate = has_gate;
    *out_all_want = all_want;
}

// ---- 生命周期事务跟踪（主线程唯一执行器；每插件至多一个在途事务）----

struct LifecycleTx {
    bool in_use = false;
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

        // 3) 资源逐条声明式安装；任一失败逆序回滚。
        const contracts::ResourceClaim* installed_claims[kMaxResourcesPerPlugin];
        uintptr_t installed_entries[kMaxResourcesPerPlugin];
        uint32_t installed_count = 0;
        bool all_gates_on = true;
        bool resource_failed = false;
        const char* failed_claim_id = nullptr;
        const char* resource_error = nullptr;

        for (uint32_t r = 0; r < plugin.resources_count; ++r) {
            const contracts::ResourceClaim& claim = plugin.resources[r];
            bool followed = false;
            const uintptr_t entry =
                deps.resolve_entry != nullptr ? deps.resolve_entry(claim.symbol, &followed) : 0;
            if (entry == 0) {
                resource_failed = true;
                failed_claim_id = claim.id;
                resource_error = "symbol unresolved";
                break;
            }
            const bool gate_on =
                claim.gate != nullptr ? infra::config_get_bool(claim.gate, false) : true;
            if (claim.gate != nullptr && !gate_on) all_gates_on = false;

            const hooks::InstallOutcome outcome =
                hooks::install_claim_entry_hook(claim, entry, deps.hook_fn, gate_on);
            if (outcome.ok != 0) {
                resource_failed = true;
                failed_claim_id = claim.id;
                resource_error = outcome.error != nullptr ? outcome.error : "install failed";
                break;
            }
            installed_claims[installed_count] = &claim;
            installed_entries[installed_count] = entry;
            ++installed_count;
            // 同步路径同样登记运行期入口：运行期 deactivate 的物理归还要用 resolved_entry。
            hooks::runtime_set(claim.id, entry, outcome.backup);
            // 立即把原函数入口交还所有者（§3.3「backup 交还」）。
            if (plugin.on_resource_ready != nullptr) {
                plugin.on_resource_ready(ctx, claim.id, outcome.backup);
            }
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

        // 4) 全部成功：带 gate 的资源全开 → enabled；否则 installed_disabled。
        const bool enabled = all_gates_on;
        infra::status_update(
            plugin.id,
            enabled ? infra::FeatureState::kEnabled : infra::FeatureState::kInstalledDisabled,
            enabled, nullptr);
        if (enabled) {
            if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
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
        bool has_gate = false;
        bool all_want = true;
        sync_plugin_resources(plugin, &has_gate, &all_want);
        if (!has_gate) continue;  // 无带 gate 资源 → 不参与门禁监督
        const bool target_enabled = all_want;
        const infra::FeatureState target_state = target_enabled
            ? infra::FeatureState::kEnabled : infra::FeatureState::kInstalledDisabled;
        if (status.state == target_state) continue;

        contracts::PluginContext ctx{};
        infra::status_update(plugin.id, target_state, target_enabled, nullptr);
        if (target_enabled) {
            if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
        } else {
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
        if (slot.kind == hooks::TxKind::kInstall && out.resource_id != nullptr &&
            out.backup != nullptr && plugin.on_resource_ready != nullptr) {
            contracts::PluginContext ctx{};
            plugin.on_resource_ready(ctx, out.resource_id, out.backup);
        }
        if (!out.finished) continue;

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
    for (LifecycleTx& slot : g_lifecycle_txs) {
        slot = LifecycleTx{};
    }
}

}  // namespace qol::host
