#include "host/plugin_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "infra/log/qol_log.h"
#include "contracts/commands.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/game_ports/custom_recipe_write_port.h"
#include "contracts/game_ports/craft_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/inventory_extras_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/item_extras_port.h"
#include "contracts/game_ports/quest_port.h"
#include "contracts/game_ports/save_call_port.h"
#include "contracts/game_ports/save_port.h"
#include "contracts/game_ports/world_port.h"
#include "contracts/ownership.h"
#include "game/access/patches.h"
#include "game/access/ptr_slots.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"
#include "host/http_dispatch.h"
#include "infra/capabilities.h"
#include "infra/config.h"
#include "infra/status.h"
#include "patch/instr_patch.h"
#include "patch/ptr_slot.h"
#if !QOL_KERNEL_ONLY
#include "game/adapter_ports/character_port.h"
#include "game/adapter_ports/character_extras_port.h"
#include "game/adapter_ports/craft_port.h"
#include "game/adapter_ports/craft_ui_write_port.h"
#include "game/adapter_ports/custom_recipe_write_port.h"
#include "game/adapter_ports/choice_port.h"
#include "game/adapter_ports/extension_bag_write_port.h"
#include "game/adapter_ports/item_port.h"
#include "game/adapter_ports/item_extras_port.h"
#include "game/adapter_ports/inventory_extras_port.h"
#include "game/adapter_ports/quest_port.h"
#include "game/adapter_ports/save_call_port.h"
#include "game/adapter_ports/save_port.h"
#include "game/adapter_ports/system_port.h"
#include "game/adapter_ports/world_port.h"
#endif

namespace qol::host {

extern const contracts::QolHostSdk g_host_sdk;
extern const contracts::QolGameHost g_game_host;

namespace {

constexpr uint32_t kMaxPlugins = 32;
constexpr uint32_t kMaxPortsPerPlugin = 8;
constexpr uint32_t kMaxErrorText = 96;
constexpr uint32_t kNoPlugin = kMaxPlugins;

struct PluginEntry {
    contracts::PluginDescriptor descriptor{};
};

PluginEntry g_plugins[kMaxPlugins];
uint32_t g_plugin_count = 0;
const void* g_plugin_ports[kMaxPlugins][kMaxPortsPerPlugin] = {};
uint32_t g_plugin_port_count[kMaxPlugins] = {};
bool g_port_invalid[kMaxPlugins] = {};

contracts::PluginContext make_plugin_context(const char* plugin_id) {
    uint32_t index = kMaxPlugins;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        if (plugin_id != nullptr &&
            std::strcmp(g_plugins[i].descriptor.id, plugin_id) == 0) {
            index = i;
            break;
        }
    }
    const void* const* ports = index == kMaxPlugins ? nullptr : g_plugin_ports[index];
    const uint32_t port_count = index == kMaxPlugins ? 0 : g_plugin_port_count[index];
    return contracts::PluginContext{sizeof(contracts::PluginContext),
                                    g_host_sdk.abi.major,
                                    g_host_sdk.abi.minor,
                                    &g_host_sdk,
                                    &g_game_host,
                                    plugin_id,
                                    ports,
                                    port_count};
}

bool register_http_queries(const contracts::PluginDescriptor& plugin) {
    if (plugin.http_query_count == 0) return true;
    return contracts::succeeded(http_dispatch_register_queries(plugin.http_queries,
                                                               plugin.http_query_count));
}

void unregister_http_queries(const contracts::PluginDescriptor& plugin) {
    if (plugin.http_query_count == 0 || plugin.id == nullptr) return;
    (void)http_dispatch_api().unregister_owner(plugin.id);
}

constexpr uint32_t kMaxResourcesPerPlugin = 32;
// 装配依赖：install_all 时存入本 TU 静态，供运行期 lifecycle_step 的受限回调失败撤回
// （以及 release 复用同一 hook/unhook 原语）；assembly.cpp 已在装配期传入。
HostDeps g_install_deps{};
extern char g_error_text[kMaxPlugins][kMaxErrorText];

struct AvailablePort {
    contracts::QolPortId id;
    QolVersion version;
    const void* iface;
};

bool available_port(contracts::QolPortId id, AvailablePort* out) {
#if !QOL_KERNEL_ONLY
    if (id == contracts::QolPortId::QOL_PORT_QUEST) {
        const auto& port = qol::game::quest_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_WORLD) {
        const auto& port = qol::game::world_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_ITEM) {
        const auto& port = qol::game::item_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS) {
        const auto& port = qol::game::character_extras_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_INVENTORY_EXTRAS) {
        const auto& port = qol::game::inventory_extras_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_ITEM_EXTRAS) {
        const auto& port = qol::game::item_extras_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_CHARACTER) {
        const auto& port = qol::game::character_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_EXTENSION_BAG_WRITE) {
        const auto& port = qol::game::extension_bag_write_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_CHOICE) {
        const auto& port = qol::game::choice_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_SAVE) {
        const auto& port = qol::game::save_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_SAVE_CALL) {
        const auto& port = qol::game::save_call_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_CRAFT) {
        const auto& port = qol::game::craft_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_CRAFT_UI_WRITE) {
        const auto& port = qol::game::craft_ui_write_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_CUSTOM_RECIPE_WRITE) {
        const auto& port = qol::game::custom_recipe_write_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
    if (id == contracts::QolPortId::QOL_PORT_SYSTEM) {
        const auto& port = qol::game::system_port();
        if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
        return true;
    }
#else
    (void)id;
    (void)out;
#endif
    return false;
}

bool resolve_plugin_ports(uint32_t index) {
    const contracts::PluginDescriptor& plugin = g_plugins[index].descriptor;
    g_plugin_port_count[index] = 0;
    if (plugin.port_count == 0) return true;
    if (plugin.ports == nullptr || plugin.port_count > kMaxPortsPerPlugin) return false;
    for (uint32_t i = 0; i < plugin.port_count; ++i) {
        const contracts::QolPortRequirement& requirement = plugin.ports[i];
        if (requirement.id == contracts::QolPortId::QOL_PORT_EXTENSION_BAG_WRITE &&
            (plugin.id == nullptr || std::strcmp(plugin.id, "extension_bag") != 0)) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]),
                          "private port: %s", contracts::port_id_name(requirement.id));
            return false;
        }
        if (requirement.id == contracts::QolPortId::QOL_PORT_CHOICE &&
            (plugin.id == nullptr ||
             (std::strcmp(plugin.id, "native_choice") != 0 &&
              std::strcmp(plugin.id, "world_teleport") != 0))) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]),
                          "private port: %s", contracts::port_id_name(requirement.id));
            return false;
        }
        if (requirement.id == contracts::QolPortId::QOL_PORT_SAVE_CALL &&
            (plugin.id == nullptr || std::strcmp(plugin.id, "save_backup") != 0)) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]),
                          "private port: %s", contracts::port_id_name(requirement.id));
            return false;
        }
        if (requirement.id == contracts::QolPortId::QOL_PORT_CRAFT &&
            (plugin.id == nullptr || std::strcmp(plugin.id, "craft_facts") != 0)) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]),
                          "private port: %s", contracts::port_id_name(requirement.id));
            return false;
        }
        if (requirement.id == contracts::QolPortId::QOL_PORT_CRAFT_UI_WRITE &&
            (plugin.id == nullptr || std::strcmp(plugin.id, "craft_ui") != 0)) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]),
                          "private port: %s", contracts::port_id_name(requirement.id));
            return false;
        }
        if (requirement.id == contracts::QolPortId::QOL_PORT_CUSTOM_RECIPE_WRITE &&
            (plugin.id == nullptr || std::strcmp(plugin.id, "custom_recipe") != 0)) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]),
                          "private port: %s", contracts::port_id_name(requirement.id));
            return false;
        }
        AvailablePort port{};
        if (!available_port(requirement.id, &port) || port.iface == nullptr) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]),
                          "unsupported port: %s", contracts::port_id_name(requirement.id));
            return false;
        }
        if (!contracts::satisfies(port.version, requirement.range)) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]),
                          "unsupported port version: %s", contracts::port_id_name(requirement.id));
            return false;
        }
        g_plugin_ports[index][i] = port.iface;
        ++g_plugin_port_count[index];
    }
    return true;
}

// 失败文本静态存储：infra/status 只保存指针，生命周期须 ≥ 进程。
char g_error_text[kMaxPlugins][kMaxErrorText];
// 门禁应用失败（fail-soft）的 degraded 记录：仅记录由本路径置位的 degraded，避免清除
// optional_requires 级联设置的 degraded；error 文本同为静态存储。
char g_gate_error_text[kMaxPlugins][kMaxErrorText];
bool g_gate_degraded[kMaxPlugins];

// 依赖图由描述符构建。注册顺序只决定描述符进入图的时机，不决定业务安装顺序；
// g_install_order 是确定性的拓扑结果，provider 必须在 consumer 前完成 install/publish。
uint32_t g_install_order[kMaxPlugins];
uint32_t g_install_order_count = 0;
bool g_dependency_invalid[kMaxPlugins];

uint32_t provider_for_requirement(const contracts::CapabilityRequirement& req,
                                  bool* out_id_found) {
    if (out_id_found != nullptr) *out_id_found = false;
    if (req.id == nullptr) return kNoPlugin;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        const contracts::PluginDescriptor& candidate = g_plugins[i].descriptor;
        for (uint32_t p = 0; p < candidate.provides_count; ++p) {
            const contracts::CapabilityProvisionRef& provision = candidate.provides[p];
            if (provision.id == nullptr || std::strcmp(provision.id, req.id) != 0) continue;
            if (out_id_found != nullptr) *out_id_found = true;
            if (contracts::satisfies(provision.version, req.range)) return i;
        }
    }
    return kNoPlugin;
}

void set_dependency_error(uint32_t index, const char* prefix, const char* capability_id) {
    std::snprintf(g_error_text[index], sizeof(g_error_text[index]), "%s: %s", prefix,
                  capability_id != nullptr ? capability_id : "<null>");
}

bool dependency_order_build() {
    bool edges[kMaxPlugins][kMaxPlugins] = {};
    uint32_t indegree[kMaxPlugins] = {};
    g_install_order_count = 0;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        g_dependency_invalid[i] = g_port_invalid[i];
    }

    // 先标记缺失/版本不兼容的 required，避免它们污染环检测结果。
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        if (g_dependency_invalid[i]) continue;
        const contracts::PluginDescriptor& plugin = g_plugins[i].descriptor;
        for (uint32_t r = 0; r < plugin.requires_count; ++r) {
            bool id_found = false;
            const uint32_t provider = provider_for_requirement(plugin.requires[r], &id_found);
            if (provider != kNoPlugin) continue;
            g_dependency_invalid[i] = true;
            set_dependency_error(i, id_found ? "incompatible required capability"
                                             : "missing required capability",
                                 plugin.requires[r].id);
            break;
        }
    }

    // required 和已注册的 optional provider 都进入 DAG。optional provider 不存在时不建边，
    // consumer 仍可按 degraded 语义安装；存在时固定先发布 provider，避免无意义的顺序依赖。
    for (uint32_t consumer = 0; consumer < g_plugin_count; ++consumer) {
        if (g_dependency_invalid[consumer]) continue;
        const contracts::PluginDescriptor& plugin = g_plugins[consumer].descriptor;
        for (uint32_t r = 0; r < plugin.requires_count; ++r) {
            const uint32_t provider = provider_for_requirement(plugin.requires[r], nullptr);
            if (provider == kNoPlugin || g_dependency_invalid[provider]) continue;
            if (!edges[provider][consumer]) {
                edges[provider][consumer] = true;
                ++indegree[consumer];
            }
        }
        for (uint32_t o = 0; o < plugin.optional_count; ++o) {
            const uint32_t provider = provider_for_requirement(plugin.optional_requires[o], nullptr);
            if (provider == kNoPlugin || g_dependency_invalid[provider] || provider == consumer) {
                continue;
            }
            if (!edges[provider][consumer]) {
                edges[provider][consumer] = true;
                ++indegree[consumer];
            }
        }
    }

    bool emitted[kMaxPlugins] = {};
    for (;;) {
        uint32_t selected = kNoPlugin;
        for (uint32_t i = 0; i < g_plugin_count; ++i) {
            if (g_dependency_invalid[i] || emitted[i] || indegree[i] != 0) continue;
            if (selected == kNoPlugin ||
                std::strcmp(g_plugins[i].descriptor.id, g_plugins[selected].descriptor.id) < 0) {
                selected = i;
            }
        }
        if (selected == kNoPlugin) break;
        emitted[selected] = true;
        g_install_order[g_install_order_count++] = selected;
        for (uint32_t consumer = 0; consumer < g_plugin_count; ++consumer) {
            if (edges[selected][consumer] && indegree[consumer] > 0) --indegree[consumer];
        }
    }

    bool valid = true;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        if (g_dependency_invalid[i]) {
            valid = false;
            continue;
        }
        if (!emitted[i]) {
            g_dependency_invalid[i] = true;
            set_dependency_error(i, "dependency cycle", g_plugins[i].descriptor.id);
            valid = false;
        }
    }
    return valid;
}

bool descriptor_valid(const contracts::PluginDescriptor& plugin) {
    if (plugin.id == nullptr || plugin.id[0] == '\0') return false;
    if (plugin.version == nullptr) return false;
    if (plugin.install == nullptr) return false;
    if (plugin.resources_count > kMaxResourcesPerPlugin) return false;
    if (plugin.resources_count > 0 && plugin.resources == nullptr) return false;
    if (plugin.http_query_count > 0 && plugin.http_queries == nullptr) return false;
    if (plugin.requires_count > 0 && plugin.requires == nullptr) return false;
    if (plugin.optional_count > 0 && plugin.optional_requires == nullptr) return false;
    if (plugin.provides_count > 0 && plugin.provides == nullptr) return false;
    if (plugin.jni_count > 0 && plugin.jni == nullptr) return false;
    if (plugin.port_count > kMaxPortsPerPlugin) return false;
    if (plugin.port_count > 0 && plugin.ports == nullptr) return false;
    return true;
}

// requires 缺失判定；返回 nullptr 表示全部满足，否则为静态错误文本。
const char* check_requires(const contracts::PluginDescriptor& plugin, uint32_t plugin_index) {
    for (uint32_t i = 0; i < plugin.requires_count; ++i) {
        const contracts::CapabilityRequirement& req = plugin.requires[i];
        if (infra::require_capability(req.id, req.range) == nullptr) {
            set_dependency_error(plugin_index, "missing required capability", req.id);
            return g_error_text[plugin_index];
        }
    }
    return nullptr;
}

const char* check_optional_requires(const contracts::PluginDescriptor& plugin, uint32_t plugin_index) {
    for (uint32_t i = 0; i < plugin.optional_count; ++i) {
        const contracts::CapabilityRequirement& req = plugin.optional_requires[i];
        if (infra::require_capability(req.id, req.range) == nullptr) {
            set_dependency_error(plugin_index, "missing optional capability", req.id);
            return g_error_text[plugin_index];
        }
    }
    return nullptr;
}

// 非入口机制的同步安装（kCallPatch / kPtrSlot / kOwnership）；成功后登记运行期地址。
// out_backup 为非 nullptr 时回填「原函数入口」：kPtrSlot 为其槽期望值（供 §3.3 backup 交还），
// 其余机制为 nullptr。返回 nullptr 成功，否则静态错误文本。
const char* install_non_entry_claim(const contracts::ResourceClaim& claim, uintptr_t* out_target,
                                    void** out_backup) {
    if (out_backup != nullptr) {
        *out_backup = nullptr;
    }
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
    // kPtrSlot：槽站点由 game/access/ptr_slots 解析（禁止裸地址），机制体在 patch/ptr_slot。
    if (claim.mechanism == contracts::ResourceMechanism::kPtrSlot) {
        qol::game::PtrSlotSite site{};
        if (!qol::game::ptr_slot_site(claim.symbol, &site)) {
            return "slot not found";
        }
        const qol::patch::Outcome outcome = qol::patch::install_claim(claim, site);
        if (outcome.ok != 0) {
            return outcome.error != nullptr ? outcome.error : "install failed";
        }
        hooks::runtime_set(claim.id, site.slot_addr, site.expected_value);
        if (out_target != nullptr) {
            *out_target = site.slot_addr;
        }
        if (out_backup != nullptr) {
            *out_backup = site.expected_value;
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

// 非入口机制的物理还原：kOwnership 调 ops->release（幂等）；kCallPatch/kPtrSlot 站点
// 由 game/access 重新解析（符号地址在进程内稳定）。
bool release_non_entry_claim(const contracts::ResourceClaim& claim) {
    if (claim.mechanism == contracts::ResourceMechanism::kOwnership) {
        contracts::ownership_release(contracts::ownership_ops(claim.handler), claim.id);
        return true;
    }
    if (claim.mechanism == contracts::ResourceMechanism::kPtrSlot) {
        qol::game::PtrSlotSite site{};
        if (!qol::game::ptr_slot_site(claim.symbol, &site)) {
            return false;
        }
        const qol::patch::Outcome outcome = qol::patch::release_claim(claim, site);
        return outcome.ok == 0;
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
            hooks::set_state(claim.id, contracts::ResourceState::kFree);
        } else {
            hooks::set_state(claim.id, contracts::ResourceState::kTombstone);
            hooks::isolated_set(claim.id, true);
        }
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

bool plugin_has_atomic_claims(const contracts::PluginDescriptor& plugin) {
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        if (plugin.resources[i].atomic) return true;
    }
    return false;
}

bool atomic_group_satisfied(const contracts::PluginDescriptor& plugin) {
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        const contracts::ResourceClaim& claim = plugin.resources[i];
        if (claim.atomic && hooks::state_of(claim.id) != contracts::ResourceState::kLive) {
            return false;
        }
    }
    return true;
}

const contracts::ResourceClaim* atomic_claim_by_id(const contracts::PluginDescriptor& plugin,
                                                   const char* resource_id) {
    if (resource_id == nullptr) return nullptr;
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        if (plugin.resources[i].atomic && std::strcmp(plugin.resources[i].id, resource_id) == 0) {
            return &plugin.resources[i];
        }
    }
    return nullptr;
}

bool atomic_group_want(const contracts::PluginDescriptor& plugin) {
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        const contracts::ResourceClaim& claim = plugin.resources[i];
        if (claim.atomic && claim.gate != nullptr &&
            !infra::config_get_bool(claim.gate, false)) {
            return false;
        }
    }
    return true;
}

// 原子组失败时释放组内所有已占用成员，而不是只释放本轮新装成员。这样上一轮
// 已 live 的成员也不会在组失败时继续对外提供部分功能。
void rollback_atomic_group(const contracts::PluginDescriptor& plugin, const HostDeps& deps,
                           bool* out_mechanism_failed) {
    for (uint32_t i = plugin.resources_count; i > 0; --i) {
        const contracts::ResourceClaim& claim = plugin.resources[i - 1];
        if (!claim.atomic) continue;
        const contracts::ResourceState state = hooks::state_of(claim.id);
        if (state == contracts::ResourceState::kLive ||
            state == contracts::ResourceState::kDisabled) {
            revoke_resource(claim.id, deps);
        }
        const contracts::ResourceState final_state = hooks::state_of(claim.id);
        if (final_state == contracts::ResourceState::kFree) {
            hooks::retry_pending_set(claim.id, true);
        } else if (final_state == contracts::ResourceState::kTombstone) {
            if (out_mechanism_failed != nullptr) *out_mechanism_failed = true;
        }
    }
}

// 以一条同步事务安装同一插件的原子组。返回 true 表示本轮出现失败；失败后组成员
// 已统一回到 free（物理回滚失败则保留 tombstone），并通过 retry_pending 进入逐帧重试。
bool install_atomic_group(const contracts::PluginDescriptor& plugin,
                          contracts::PluginContext& ctx, const HostDeps& deps,
                          const contracts::ResourceClaim** installed_claims,
                          uintptr_t* installed_entries, uint32_t* installed_count,
                          bool* out_mechanism_failed, const char** out_failed_id,
                          const char** out_error) {
    if (!plugin_has_atomic_claims(plugin)) return false;

    const bool group_want = atomic_group_want(plugin);
    const char* failed_id = nullptr;
    const char* error = nullptr;
    bool failed = false;
    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
        const contracts::ResourceClaim& claim = plugin.resources[i];
        if (!claim.atomic) continue;
        const contracts::ResourceState state = hooks::state_of(claim.id);
        if (state == contracts::ResourceState::kLive ||
            state == contracts::ResourceState::kDisabled) {
            continue;
        }
        // 非入口资源在门禁关闭时没有空转态；整组保持 free，待门禁开启后再尝试。
        if (!group_want && claim.mechanism != contracts::ResourceMechanism::kEntryHook) {
            continue;
        }
        if (state != contracts::ResourceState::kFree) {
            failed = true;
            failed_id = claim.id;
            error = "resource not free";
            break;
        }

        if (claim.mechanism == contracts::ResourceMechanism::kEntryHook) {
            bool followed = false;
            const uintptr_t entry = deps.resolve_entry != nullptr
                                        ? deps.resolve_entry(claim.symbol, &followed)
                                        : 0;
            if (entry == 0) {
                failed = true;
                failed_id = claim.id;
                error = "symbol unresolved";
                break;
            }
            const hooks::InstallOutcome outcome = hooks::install_claim_entry_hook(
                claim, entry, deps.hook_fn, false);
            if (outcome.ok != 0) {
                failed = true;
                failed_id = claim.id;
                error = outcome.error != nullptr ? outcome.error : "install failed";
                break;
            }
            hooks::runtime_set(claim.id, entry, outcome.backup);
            if (plugin.on_resource_ready != nullptr &&
                plugin.on_resource_ready(ctx, claim.id, outcome.backup) != 0) {
                failed = true;
                failed_id = claim.id;
                error = "resource_ready_failed";
                break;
            }
            if (installed_claims != nullptr && installed_entries != nullptr &&
                installed_count != nullptr) {
                installed_claims[*installed_count] = &claim;
                installed_entries[*installed_count] = entry;
                ++*installed_count;
            }
            continue;
        }

        uintptr_t target = 0;
        void* backup = nullptr;
        const char* install_error = install_non_entry_claim(claim, &target, &backup);
        if (install_error != nullptr) {
            failed = true;
            failed_id = claim.id;
            error = install_error;
            break;
        }
        // 非入口机制没有空转态；原子组仍先保持非 live 登记，待全部成员完成后统一
        // 发布，失败路径由 rollback_atomic_group 物理释放。
        hooks::set_state(claim.id, contracts::ResourceState::kDisabled);
        hooks::retry_pending_set(claim.id, false);
        if (installed_claims != nullptr && installed_entries != nullptr &&
            installed_count != nullptr) {
            installed_claims[*installed_count] = &claim;
            installed_entries[*installed_count] = target;
            ++*installed_count;
        }
    }

    if (failed) {
        rollback_atomic_group(plugin, deps, out_mechanism_failed);
        if (out_failed_id != nullptr) *out_failed_id = failed_id;
        if (out_error != nullptr) *out_error = error;
        return true;
    }

    // 门禁从关闭切换为开启时，entry hook 已可能处于 disabled；只有整组走到这里
    // 才把它们统一切为 live，避免中间态触发功能 wrapper。
    if (group_want) {
        for (uint32_t i = 0; i < plugin.resources_count; ++i) {
            const contracts::ResourceClaim& claim = plugin.resources[i];
            if (claim.atomic && hooks::state_of(claim.id) == contracts::ResourceState::kDisabled) {
                hooks::set_state(claim.id, contracts::ResourceState::kLive);
            }
            if (claim.atomic) hooks::retry_pending_set(claim.id, false);
        }
    }
    return false;
}

// 常驻（gate==nullptr）fail-soft/optional 资源失败后的定向重试（§8：保持 free、可重试）。
// 安装语义与 install_all 的一次性安装一致：入口 hook 安装成功后 runtime_set（顺带清除
// retry_pending）并把 backup 交还所有者（wrapper 依赖该 backup 重放原函数）；非入口机制
// 走 install_non_entry_claim。返回 nullptr 成功，否则静态错误文本。
// 受限回调失败：物理安装已成立但 backup 未交还，wrapper 不可用，故撤回本条并保持待重试。
const char* retry_resident_claim(const contracts::PluginDescriptor& plugin,
                                 contracts::PluginContext& ctx,
                                 const contracts::ResourceClaim& claim, const HostDeps& deps) {
    if (claim.mechanism == contracts::ResourceMechanism::kEntryHook) {
        bool followed = false;
        const uintptr_t entry =
            deps.resolve_entry != nullptr ? deps.resolve_entry(claim.symbol, &followed) : 0;
        if (entry == 0) return "symbol unresolved";
        const hooks::InstallOutcome outcome =
            hooks::install_claim_entry_hook(claim, entry, deps.hook_fn, true);
        if (outcome.ok != 0) {
            return outcome.error != nullptr ? outcome.error : "install failed";
        }
        hooks::runtime_set(claim.id, entry, outcome.backup);
        if (plugin.on_resource_ready != nullptr &&
            plugin.on_resource_ready(ctx, claim.id, outcome.backup) != 0) {
            revoke_resource(claim.id, deps);
            if (hooks::state_of(claim.id) == contracts::ResourceState::kFree) {
                hooks::retry_pending_set(claim.id, true);
            }
            return "resource_ready_failed";
        }
        return nullptr;
    }
    uintptr_t target = 0;
    void* backup = nullptr;
    const char* error = install_non_entry_claim(claim, &target, &backup);
    if (error != nullptr) return error;
    // 非入口机制成功：与 install_all 一致置 live；kOwnership 无运行期记录，标记显式清除。
    hooks::set_state(claim.id, contracts::ResourceState::kLive);
    hooks::retry_pending_set(claim.id, false);
    if (backup != nullptr && plugin.on_resource_ready != nullptr &&
        plugin.on_resource_ready(ctx, claim.id, backup) != 0) {
        revoke_resource(claim.id, deps);
        if (hooks::state_of(claim.id) == contracts::ResourceState::kFree) {
            hooks::retry_pending_set(claim.id, true);
        }
        return "resource_ready_failed";
    }
    return nullptr;
}

// 门禁监督辅助：同步某插件带 gate 资源的登记状态，并汇总目标态。
// 入口 hook 只改写 disabled/live 两态：free（未安装）与 tombstone（保留占用）不得被监督改写。
// 非入口机制（call patch / ownership）不支持空转（§4.2 规则 6）：门禁变化直接物理安装/还原。
// 安装失败按 §8 逐资源策略处置（与 install_all 一致）：
//   · kRequired×kFailClosed → tombstone + isolated + mechanism_failed（不可重试，插件 failed）；
//   · 其余（kOptional×任一 / kRequired×kFailSoft）→ 保持 free 供下一逻辑帧重试，
//     经 out_degraded/out_error_* 记录原因（degraded，不改生命周期态）。
// 释放/还原失败仍是不可重试形态 → tombstone + isolated（§4.2）。
// 另处理 gate==nullptr 常驻资源的定向重试：安装期 fail-soft/optional 失败会置
// retry_pending，此处对「已置位且仍 free」的条目按同一失败分流重装（成功清标记与
// degraded、失败保持 degraded）；未置位者不重装，维持一次性安装语义。
// 同步执行原因：单一指令字改写为同步 fail-closed 原语，且没有在途 wrapper 需等待；
// 生命周期安装/还原仍走事务引擎（lifecycle_apply/lifecycle_step）。
void sync_plugin_resources(const contracts::PluginDescriptor& plugin, const HostDeps& deps,
                           bool* out_has_gate, bool* out_all_want, bool* out_mechanism_failed,
                           bool* out_degraded, const char** out_error_claim,
                           const char** out_error_reason) {
    bool has_gate = false;
    bool all_want = true;
    bool mechanism_failed = false;
    bool degraded = false;
    const char* error_claim = nullptr;
    const char* error_reason = nullptr;
    // 本次门禁同步新装成功的非入口资源；失败时逆序还原，禁止半装对外可见。
    const contracts::ResourceClaim* installed[kMaxResourcesPerPlugin];
    uintptr_t installed_entries[kMaxResourcesPerPlugin];
    uint32_t installed_count = 0;
    const bool has_atomic = plugin_has_atomic_claims(plugin);
    const bool atomic_want = atomic_group_want(plugin);
    if (has_atomic && atomic_want) {
        contracts::PluginContext atomic_ctx = make_plugin_context(plugin.id);
        const bool atomic_failed = install_atomic_group(
            plugin, atomic_ctx, deps, installed, installed_entries,
            &installed_count,
            &mechanism_failed, &error_claim, &error_reason);
        if (atomic_failed) {
            if (mechanism_failed) {
                error_claim = nullptr;
                error_reason = nullptr;
            } else {
                degraded = true;
            }
            installed_count = 0;
        }
    }
    for (uint32_t r = 0; r < plugin.resources_count; ++r) {
        const contracts::ResourceClaim& claim = plugin.resources[r];
        if (claim.atomic && atomic_want) continue;
        if (claim.gate == nullptr) {
            // 常驻 fail-soft/optional 资源的定向重试：仅处理本路径置位且仍为 free 的条目；
            // 未置位者保持一次性安装语义，不做无谓重装。
            if (!hooks::retry_pending_get(claim.id)) continue;
            if (hooks::state_of(claim.id) != contracts::ResourceState::kFree) continue;
            contracts::PluginContext retry_ctx = make_plugin_context(plugin.id);
            const char* retry_error = retry_resident_claim(plugin, retry_ctx, claim, deps);
            if (retry_error != nullptr) {
                const bool fail_closed_required =
                    claim.requirement == contracts::ResourceRequirement::kRequired &&
                    claim.failure == contracts::ResourceFailure::kFailClosed;
                if (fail_closed_required) {
                    mechanism_failed = true;
                } else {
                    degraded = true;
                    error_claim = claim.id;
                    error_reason = retry_error;
                }
            } else {
                installed[installed_count++] = &claim;
            }
            continue;
        }
        has_gate = true;
        const bool want = infra::config_get_bool(claim.gate, false);
        if (!want) all_want = false;
        const contracts::ResourceState now = hooks::state_of(claim.id);
        if (claim.mechanism == contracts::ResourceMechanism::kEntryHook) {
            if (want && now == contracts::ResourceState::kDisabled) {
                hooks::set_state(claim.id, contracts::ResourceState::kLive);
            } else if (!want && now == contracts::ResourceState::kLive) {
                hooks::set_state(claim.id, contracts::ResourceState::kDisabled);
            } else if (want && now == contracts::ResourceState::kFree) {
                // 装配期 fail-soft 未装成功：门禁每逻辑帧重试到成功（§8；语义等价旧
                // 300ms 重试线程，且状态可观测）。安装成功立即交还 backup；受限回调
                // 失败撤回本条并保持 degraded，供下一逻辑帧再试。
                bool followed = false;
                const uintptr_t entry =
                    deps.resolve_entry != nullptr ? deps.resolve_entry(claim.symbol, &followed) : 0;
                if (entry == 0) {
                    degraded = true;
                    error_claim = claim.id;
                    error_reason = "symbol unresolved";
                } else {
                    const hooks::InstallOutcome outcome =
                        hooks::install_claim_entry_hook(claim, entry, deps.hook_fn, true);
                    if (outcome.ok != 0) {
                        degraded = true;
                        error_claim = claim.id;
                        error_reason = outcome.error != nullptr ? outcome.error : "install failed";
                    } else {
                        hooks::runtime_set(claim.id, entry, outcome.backup);
                        bool ready_ok = true;
                        if (plugin.on_resource_ready != nullptr) {
                            contracts::PluginContext ready_ctx = make_plugin_context(plugin.id);
                            ready_ok =
                                plugin.on_resource_ready(ready_ctx, claim.id, outcome.backup) == 0;
                        }
                        if (!ready_ok) {
                            revoke_resource(claim.id, deps);
                            degraded = true;
                            error_claim = claim.id;
                            error_reason = "resource_ready_failed";
                        } else {
                            installed[installed_count++] = &claim;
                        }
                    }
                }
            }
            continue;
        }
        if (want && now == contracts::ResourceState::kFree) {
            uintptr_t target = 0;
            // 门禁路径不交还 backup：本路径仅服务带 gate 的资源，其安装点由 sync_gates
            // 逐帧监督，无启动期 on_resource_ready 语义（当前无 kPtrSlot 带 gate 资源）。
            const char* error = install_non_entry_claim(claim, &target, nullptr);
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
        // fail-closed 只标记本轮事务失败，不提前中止遍历；其余新装资源也纳入
        // installed，循环结束后统一逆序回滚，fail-soft 资源则保持 free/degraded 继续。
        for (uint32_t i = installed_count; i > 0; --i) {
            const contracts::ResourceClaim& claim = *installed[i - 1];
            if (claim.mechanism == contracts::ResourceMechanism::kEntryHook) {
                // 门禁重试新装的入口 hook：走统一撤回（free 或 tombstone+isolated）。
                revoke_resource(claim.id, deps);
                continue;
            }
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
    bool cascade = false;       // 级联撤销：成功后保持 installed_disabled，不调用 deactivate
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

// ---- 能力注册与撤销级联（§3.4.1 / §3.4.4）----

enum class CapRegistration : uint8_t { kNone = 0, kRegistered = 1, kRevoking = 2 };
CapRegistration g_cap_registration[kMaxPlugins];

struct CascadeContext {
    bool visited[kMaxPlugins] = {};
};

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

void cascade_dependents(const char* capability_id, CascadeContext& context);

bool schedule_release(uint32_t index, bool cascade) {
    if (index >= g_plugin_count) return false;
    const contracts::PluginDescriptor& plugin = g_plugins[index].descriptor;
    LifecycleTx* slot = nullptr;
    for (LifecycleTx& candidate : g_lifecycle_txs) {
        if (!candidate.in_use) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) return false;
    const uint64_t tx_id = hooks::tx_begin(plugin.id, hooks::TxKind::kRelease);
    if (tx_id == 0) return false;
    slot->in_use = true;
    slot->ready_failed = false;
    slot->cascade = cascade;
    slot->tx_id = tx_id;
    slot->plugin_index = index;
    slot->kind = hooks::TxKind::kRelease;
    return true;
}

// 级联停用 required 消费者（§3.4.1 第 3 条）：关消费者门禁 → 释放资源 →
// 递归撤销其自身提供的能力 → 置 installed_disabled + reason=cascade_stopped。
void cascade_stop_plugin(uint32_t index, CascadeContext& context) {
    if (index >= g_plugin_count || context.visited[index]) return;
    context.visited[index] = true;
    const contracts::PluginDescriptor& plugin = g_plugins[index].descriptor;
    unregister_http_queries(plugin);
    if (plugin.on_disabled != nullptr) {
        contracts::PluginContext ctx = make_plugin_context(plugin.id);
        plugin.on_disabled(ctx);
    }
    // 释放消费者资源：走与 deactivate 相同的帧步进还原事务，等待在途归零。
    if (plugin.resources_count == 0) {
        infra::status_update(plugin.id, infra::FeatureState::kInstalledDisabled, false,
                             "cascade_stopped");
    } else if (schedule_release(index, true)) {
        infra::status_update(plugin.id, infra::FeatureState::kDeactivating, false, nullptr);
    } else {
        infra::status_update(plugin.id, infra::FeatureState::kFailed, false,
                             "cascade release unavailable");
    }
    // 消费者自身提供的能力：注销并继续级联（递归即逆拓扑序）。
    if (g_cap_registration[index] == CapRegistration::kRegistered) {
        g_cap_registration[index] = CapRegistration::kRevoking;
        for (uint32_t i = 0; i < plugin.provides_count; ++i) {
            cascade_dependents(plugin.provides[i].id, context);
        }
        unregister_provided_capabilities(index);
    }
}

// 依赖失效级联：requires 命中 → 逆拓扑停用；optional_requires 命中 → degraded 标记。
void cascade_dependents(const char* capability_id, CascadeContext& context) {
    if (capability_id == nullptr) return;
    // 依赖图的逆拓扑序保证更深层 consumer 先于 provider 侧 consumer 进入撤销事务；
    // 每层仍由 capability_id 精确筛选，避免 optional 依赖被 required 级联停用。
    for (uint32_t order_pos = g_install_order_count; order_pos > 0; --order_pos) {
        const uint32_t i = g_install_order[order_pos - 1];
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
                cascade_stop_plugin(i, context);
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
    unregister_http_queries(plugin);
    if (plugin.on_disabled != nullptr) {
        contracts::PluginContext ctx = make_plugin_context(plugin.id);
        plugin.on_disabled(ctx);
    }
    if (g_cap_registration[index] != CapRegistration::kRegistered) return;
    g_cap_registration[index] = CapRegistration::kRevoking;
    CascadeContext context{};
    context.visited[index] = true;
    for (uint32_t i = 0; i < plugin.provides_count; ++i) {
        cascade_dependents(plugin.provides[i].id, context);
    }
    unregister_provided_capabilities(index);
}

// 同步完成一个已通过 required 校验的插件安装。装配期与 blocked 重评估共用这条路径，
// 保持资源安装、受限回调失败回滚、门禁落态和能力发布语义一致。
bool install_plugin_sync(uint32_t index, const HostDeps& deps) {
    const contracts::PluginDescriptor& plugin = g_plugins[index].descriptor;
    contracts::PluginContext ctx = make_plugin_context(plugin.id);

    if (plugin.install(ctx) != 0) {
        infra::status_update(plugin.id, infra::FeatureState::kFailed, false, "install failed");
        return false;
    }

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
        // 常驻（gate==nullptr）fail-soft/optional 失败：资源保持 free，置 retry_pending
        // 供 sync_gates 每逻辑帧重试（带 gate 资源由门禁路径自行重试，无需标记）。
        if (claim.gate == nullptr) {
            hooks::retry_pending_set(claim.id, true);
        }
        degraded = true;
        degraded_claim_id = claim.id;
        degraded_error = error;
        return false;
    };

    // 原子组先作为一个整体尝试；组内失败由专用回滚处理，避免后续普通资源路径
    // 把已经回滚的成员再次当作本轮已装资源释放。
    if (plugin_has_atomic_claims(plugin)) {
        const bool atomic_failed = install_atomic_group(
            plugin, ctx, deps, installed_claims, installed_entries, &installed_count,
            &resource_failed, &failed_claim_id, &resource_error);
        if (atomic_failed) {
            const bool rollback_failed = resource_failed;
            installed_count = 0;
            if (!rollback_failed) {
                degraded = true;
                degraded_claim_id = failed_claim_id;
                degraded_error = resource_error;
                failed_claim_id = nullptr;
                resource_error = nullptr;
            }
        }
    }

    for (uint32_t r = 0; r < plugin.resources_count; ++r) {
        const contracts::ResourceClaim& claim = plugin.resources[r];
        const bool gate_on =
            claim.gate != nullptr ? infra::config_get_bool(claim.gate, false) : true;
        if (claim.gate != nullptr && !gate_on) all_gates_on = false;

        // 原子成员已由上面的组事务处理；组失败时不得落入逐条安装路径。
        if (claim.atomic) continue;

        // 非入口机制（call patch / ptr slot / ownership）不支持空转：门禁关闭不安装，
        // 登记保持 free。
        if (claim.mechanism != contracts::ResourceMechanism::kEntryHook) {
            if (claim.gate != nullptr && !gate_on) continue;
            uintptr_t target = 0;
            void* backup = nullptr;
            const char* error = install_non_entry_claim(claim, &target, &backup);
            if (error != nullptr) {
                if (handle_claim_failure(claim, error)) break;
                continue;
            }
            hooks::set_state(claim.id, contracts::ResourceState::kLive);
            installed_claims[installed_count] = &claim;
            installed_entries[installed_count] = target;
            ++installed_count;
            // kPtrSlot 的原函数入口 = 槽期望值，立即交还所有者；其余机制无 backup。
            if (backup != nullptr && plugin.on_resource_ready != nullptr &&
                plugin.on_resource_ready(ctx, claim.id, backup) != 0) {
                rollback_installed(installed_claims, installed_count, deps);
                infra::status_update(plugin.id, infra::FeatureState::kFailed, false,
                                     "resource_ready_failed");
                QOL_LOG_WARN(QolDomain::kCore, "plugin %s resource_ready_failed on %s",
                             plugin.id, claim.id);
                plugin_aborted = true;
                break;
            }
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
        hooks::runtime_set(claim.id, entry, outcome.backup);
        // backup 交还失败按逆序同步撤回本插件全部已装资源，禁止半装对外可见。
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
            plugin_aborted = true;
            break;
        }
        installed_claims[installed_count] = &claim;
        installed_entries[installed_count] = entry;
        ++installed_count;
    }

    if (plugin_aborted) return false;

    if (resource_failed && failed_claim_id == nullptr && installed_count == 0) {
        // 原子组物理回滚失败时已完成状态落盘，直接保持 failed。
        infra::status_update(plugin.id, infra::FeatureState::kFailed, false,
                             "atomic group rollback failed");
        return false;
    }

    if (resource_failed) {
        rollback_resources(installed_claims, installed_entries, installed_count, deps);
        std::snprintf(g_error_text[index], sizeof(g_error_text[index]), "resource %s: %s",
                      failed_claim_id != nullptr ? failed_claim_id : "?",
                      resource_error != nullptr ? resource_error : "failed");
        infra::status_update(plugin.id, infra::FeatureState::kFailed, false, g_error_text[index]);
        QOL_LOG_WARN(QolDomain::kCore, "plugin %s install failed: %s", plugin.id,
                     g_error_text[index]);
        return false;
    }

    // 全部成功（含 fail-soft 跳过）：带 gate 的资源全开 → enabled；否则 installed_disabled。
    const char* optional_error = check_optional_requires(plugin, index);
    if (optional_error != nullptr) {
        degraded = true;
        // check_optional_requires 已把诊断文本写入 g_error_text；避免用同一缓冲区
        // 作为 snprintf 的 source/destination，保留 capability id。
        if (degraded_claim_id == nullptr) degraded_error = nullptr;
    }
    const bool enabled = all_gates_on && atomic_group_satisfied(plugin);
    const char* status_error = nullptr;
    if (degraded) {
        if (degraded_claim_id != nullptr) {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]), "resource %s: %s",
                          degraded_claim_id, degraded_error != nullptr ? degraded_error : "failed");
        } else if (g_error_text[index][0] == '\0') {
            std::snprintf(g_error_text[index], sizeof(g_error_text[index]), "%s",
                          degraded_error != nullptr ? degraded_error : "degraded");
        }
        status_error = g_error_text[index];
        infra::status_set_degraded(plugin.id, true);
        // 记录由本路径置位，供 sync_gates 在资源重试成功后清除标记。
        g_gate_degraded[index] = true;
        QOL_LOG_WARN(QolDomain::kCore, "plugin %s degraded on %s: %s", plugin.id,
                     degraded_claim_id != nullptr ? degraded_claim_id : "?", status_error);
    }
    infra::status_update(plugin.id,
                         enabled ? infra::FeatureState::kEnabled
                                 : infra::FeatureState::kInstalledDisabled,
                         enabled, status_error);
    if (enabled) {
        if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
        if (!register_http_queries(plugin)) {
            if (plugin.on_disabled != nullptr) plugin.on_disabled(ctx);
            infra::status_update(plugin.id, infra::FeatureState::kFailed, false,
                                 "http handler registration failed");
            return false;
        }
        register_provided_capabilities(index);
    } else if (plugin.on_disabled != nullptr) {
        plugin.on_disabled(ctx);
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
    g_install_deps = deps;
    int failed_plugins = 0;
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        const contracts::PluginDescriptor& plugin = g_plugins[i].descriptor;
        infra::status_register(plugin.id, plugin.version);
    }

    // 先构建完整 capability DAG，再按拓扑顺序 install/publish。这样 provider 即使晚于
    // consumer 注册，也不会先被误判为 blocked；缺失 required 与环在这里留下可诊断状态。
    std::memset(g_error_text, 0, sizeof(g_error_text));
    std::memset(g_port_invalid, 0, sizeof(g_port_invalid));
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        if (!resolve_plugin_ports(i)) g_port_invalid[i] = true;
    }
    (void)dependency_order_build();
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        infra::status_update(g_plugins[i].descriptor.id, infra::FeatureState::kResolving, false,
                             nullptr);
    }
    for (uint32_t i = 0; i < g_plugin_count; ++i) {
        if (!g_dependency_invalid[i]) continue;
        const contracts::PluginDescriptor& plugin = g_plugins[i].descriptor;
        const bool cycle = g_error_text[i][0] != '\0' &&
                           std::strncmp(g_error_text[i], "dependency cycle", 16) == 0;
        infra::status_update(plugin.id, cycle ? infra::FeatureState::kFailed
                                             : infra::FeatureState::kBlocked,
                             false, g_error_text[i]);
        ++failed_plugins;
    }

    for (uint32_t order_pos = 0; order_pos < g_install_order_count; ++order_pos) {
        const uint32_t i = g_install_order[order_pos];
        const contracts::PluginDescriptor& plugin = g_plugins[i].descriptor;

        // 1) requires 校验：provider 未启用/安装失败 → blocked，并保留具体能力 id。
        const char* blocked_error = check_requires(plugin, i);
        if (blocked_error != nullptr) {
            infra::status_update(plugin.id, infra::FeatureState::kBlocked, false, blocked_error);
            ++failed_plugins;
            continue;
        }

        if (!install_plugin_sync(i, deps)) {
            ++failed_plugins;
        }
    }
    return failed_plugins;
}

uint32_t sync_gates() {
    uint32_t changed = 0;
    bool processed[kMaxPlugins] = {};
    for (uint32_t order_pos = 0; order_pos < g_install_order_count + g_plugin_count;
         ++order_pos) {
        // 依赖拓扑优先，确保 provider 在同一轮先发布能力；拓扑无效的插件随后仍需
        // 保留其 blocked 诊断与重评估机会。
        const uint32_t i = order_pos < g_install_order_count
                               ? g_install_order[order_pos]
                               : order_pos - g_install_order_count;
        if (processed[i]) continue;
        processed[i] = true;
        const contracts::PluginDescriptor& plugin = g_plugins[i].descriptor;
        infra::FeatureStatus status{};
        if (!infra::status_get(plugin.id, &status)) continue;
        // blocked 是可补齐的依赖等待态：能力在本轮由 provider 发布后，立即按装配期
        // 的同步安装路径重试。error 文本不变时不重复写状态，避免每帧制造无效更新。
        if (status.state == infra::FeatureState::kBlocked) {
            if (plugin_isolated(plugin) || plugin_has_active_tx(plugin.id)) continue;
            char previous_error[kMaxErrorText] = {};
            if (status.error != nullptr) {
                std::snprintf(previous_error, sizeof(previous_error), "%s", status.error);
            }
            const char* blocked_error = check_requires(plugin, i);
            if (blocked_error != nullptr) {
                if (std::strcmp(previous_error, blocked_error) != 0) {
                    infra::status_update(plugin.id, infra::FeatureState::kBlocked, false,
                                         blocked_error);
                    ++changed;
                }
                continue;
            }
            infra::status_update(plugin.id, infra::FeatureState::kResolving, false, nullptr);
            (void)install_plugin_sync(i, g_install_deps);
            ++changed;
            continue;
        }
        // 只监督已按门禁落态的插件；failed/invalid 等不参与（避免覆盖失败态）。
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
        // 撤销先于关资源（§3.4.4）：provider 由 enabled 转 disable 时先关提供者门禁并级联注销。
        bool provider_revoke_started = false;
        if (status.state == infra::FeatureState::kEnabled && !plugin_all_gates_want(plugin) &&
            plugin_provides_capability(plugin)) {
            begin_revoke_provider(i);
            provider_revoke_started = true;
        }

        bool has_gate = false;
        bool all_want = true;
        bool mechanism_failed = false;
        bool gate_degraded = false;
        const char* gate_error_claim = nullptr;
        const char* gate_error_reason = nullptr;
        sync_plugin_resources(plugin, g_install_deps, &has_gate, &all_want, &mechanism_failed,
                              &gate_degraded, &gate_error_claim, &gate_error_reason);
        if (mechanism_failed) {
            // 机制门禁应用失败（fail-closed 安装失败 / 释放失败）：保留占用并隔离，插件置 failed。
            g_gate_degraded[i] = false;
            infra::status_update(plugin.id, infra::FeatureState::kFailed, false,
                                 "mechanism gate apply failed");
            ++changed;
            continue;
        }
        const bool target_enabled = all_want && atomic_group_satisfied(plugin);
        const infra::FeatureState target_state = target_enabled
            ? infra::FeatureState::kEnabled : infra::FeatureState::kInstalledDisabled;

        // 原子组失败同样会使 provider 失效；撤销必须先于状态转移，避免能力在组不完整
        // 时继续可见。门禁关闭路径已在上方完成撤销，避免重复 on_disabled。
        if (status.state == infra::FeatureState::kEnabled && !target_enabled &&
            plugin_provides_capability(plugin) && !provider_revoke_started) {
            begin_revoke_provider(i);
            provider_revoke_started = true;
        }

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
            contracts::PluginContext ctx = make_plugin_context(plugin.id);
            if (status.state != target_state) {
                infra::status_update(plugin.id, target_state, target_enabled, g_gate_error_text[i]);
                if (target_enabled) {
                    if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
                    (void)register_http_queries(plugin);
                    register_provided_capabilities(i);
                } else if (!plugin_provides_capability(plugin)) {
                    unregister_http_queries(plugin);
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

        contracts::PluginContext ctx = make_plugin_context(plugin.id);
        infra::status_update(plugin.id, target_state, target_enabled, nullptr);
        if (target_enabled) {
            if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
            (void)register_http_queries(plugin);
            register_provided_capabilities(i);
        } else if (!plugin_provides_capability(plugin)) {
            // provider 的 on_disabled 已由 begin_revoke_provider 调用（避免重复）。
            unregister_http_queries(plugin);
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

    contracts::PluginContext ctx = make_plugin_context(plugin->id);
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
                if (plugin->resources_count == 0) {
                    const uint32_t index = plugin_index_of(plugin->id);
                    begin_revoke_provider(index);
                    // 无 provides 的 owner 走不到 begin_revoke_provider 的摘除分支；
                    // deactivate 必须置路由墓碑，保证停用态错误语义唯一（http owner inactive）。
                    unregister_http_queries(*plugin);
                    if (plugin->deactivate != nullptr) plugin->deactivate(ctx);
                    infra::status_update(plugin->id, infra::FeatureState::kInactive, false, nullptr);
                    return true;
                }
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
                slot.ready_failed = false;
                slot.cascade = false;
                slot.tx_id = tx_id;
                slot.plugin_index = index;
                slot.kind = hooks::TxKind::kRelease;
                return true;
            }
            return false;  // 槽位与插件同容量，正常不可达
        }

        case contracts::kFeatureLifecycleActivate: {
            // §3.2：inactive → resolving 为幂等重入；blocked/failed → resolving 为重试
            // （隔离态已在函数前置拒绝中拦截，未隔离的失败可安全重装）。
            // 级联停用复用 installed_disabled，但由 cascade_stopped 标记区分门禁关闭态；
            // 显式 activate 必须重新进入同一安装事务路径。
            const bool cascade_stopped =
                status.state == infra::FeatureState::kInstalledDisabled &&
                status.error != nullptr &&
                std::strcmp(status.error, "cascade_stopped") == 0;
            if (status.state != infra::FeatureState::kInactive &&
                status.state != infra::FeatureState::kBlocked &&
                status.state != infra::FeatureState::kFailed && !cascade_stopped) {
                return false;
            }
            infra::status_update(plugin->id, infra::FeatureState::kResolving, false, nullptr);
            const uint32_t index = plugin_index_of(plugin->id);
            const char* blocked_error = check_requires(*plugin, index);
            if (blocked_error != nullptr) {
                infra::status_update(plugin->id, infra::FeatureState::kBlocked, false,
                                     blocked_error);
                return false;
            }
            // §3.3：install 只做插件自身初始化（幂等重入）；失败即 failed，不启动事务。
            if (plugin->install != nullptr && plugin->install(ctx) != 0) {
                infra::status_update(plugin->id, infra::FeatureState::kFailed, false,
                                     "plugin install failed");
                return false;
            }
            if (plugin->resources_count == 0) {
                const bool enabled = plugin_all_gates_want(*plugin);
                infra::status_update(
                    plugin->id,
                    enabled ? infra::FeatureState::kEnabled
                            : infra::FeatureState::kInstalledDisabled,
                    enabled, nullptr);
                if (enabled) {
                    if (plugin->on_enabled != nullptr) plugin->on_enabled(ctx);
                    (void)register_http_queries(*plugin);
                    register_provided_capabilities(plugin_index_of(plugin->id));
                } else if (plugin->on_disabled != nullptr) {
                    unregister_http_queries(*plugin);
                    plugin->on_disabled(ctx);
                }
                return true;
            }
            const uint64_t tx_id = hooks::tx_begin(plugin->id, hooks::TxKind::kInstall);
            if (tx_id == 0) {
                infra::status_update(plugin->id, infra::FeatureState::kInactive, false, nullptr);
                return false;
            }
            for (LifecycleTx& slot : g_lifecycle_txs) {
                if (slot.in_use) continue;
                slot.in_use = true;
                slot.ready_failed = false;
                slot.cascade = false;
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

        // 生命周期事务跨帧推进时，原子成员即使已物理安装也先保持非 live 登记；
        // 下一成员失败或事务仍在途时，能力与 wrapper 不得观察到部分组。
        if (slot.kind == hooks::TxKind::kInstall && out.resource_id != nullptr && out.ok == 0 &&
            atomic_claim_by_id(plugin, out.resource_id) != nullptr) {
            hooks::set_state(out.resource_id, contracts::ResourceState::kDisabled);
        }

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
                contracts::PluginContext ctx = make_plugin_context(plugin.id);
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
            slot.cascade = false;
            slot.tx_id = 0;
            continue;
        }

        hooks::TxStatus status{};
        const bool have_status = hooks::tx_query(slot.tx_id, &status);
        const bool succeeded = have_status && status.state == hooks::TxState::kSucceeded;
        contracts::PluginContext ctx = make_plugin_context(plugin.id);
        if (succeeded) {
            if (slot.kind == hooks::TxKind::kInstall) {
                if (plugin_all_gates_want(plugin)) {
                    for (uint32_t i = 0; i < plugin.resources_count; ++i) {
                        const contracts::ResourceClaim& claim = plugin.resources[i];
                        if (claim.atomic &&
                            hooks::state_of(claim.id) == contracts::ResourceState::kDisabled) {
                            hooks::set_state(claim.id, contracts::ResourceState::kLive);
                        }
                    }
                }
                const bool enabled = plugin_all_gates_want(plugin) && atomic_group_satisfied(plugin);
                const uint32_t plugin_index = slot.plugin_index;
                if (enabled && g_gate_degraded[plugin_index]) {
                    g_gate_degraded[plugin_index] = false;
                    infra::status_set_degraded(plugin.id, false);
                }
                infra::status_update(
                    plugin.id,
                    enabled ? infra::FeatureState::kEnabled
                            : infra::FeatureState::kInstalledDisabled,
                    enabled, nullptr);
                if (enabled) {
                    if (plugin.on_enabled != nullptr) plugin.on_enabled(ctx);
                    (void)register_http_queries(plugin);
                    // §3.4.4：install 成功后进入 enabled 时注册能力。
                    register_provided_capabilities(slot.plugin_index);
                } else {
                    unregister_http_queries(plugin);
                    if (plugin.on_disabled != nullptr) plugin.on_disabled(ctx);
                }
            } else if (slot.cascade) {
                infra::status_update(plugin.id, infra::FeatureState::kInstalledDisabled, false,
                                     "cascade_stopped");
            } else {
                // §3.3：deactivate 只做插件自身清理（幂等）；资源已逆序还原，随后置 inactive。
                // kRelease 事务路径与零资源快速路径一致：先置路由墓碑再清理，停用态错误语义唯一。
                unregister_http_queries(plugin);
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
            const contracts::ResourceClaim* failed_atomic =
                plugin_has_atomic_claims(plugin)
                    ? atomic_claim_by_id(plugin, have_status ? status.failed_resource_id : nullptr)
                    : nullptr;
            if (failed_atomic != nullptr) {
                bool rollback_failed = false;
                rollback_atomic_group(plugin, g_install_deps, &rollback_failed);
                if (!rollback_failed) {
                    std::snprintf(g_error_text[slot.plugin_index],
                                  sizeof(g_error_text[slot.plugin_index]), "resource %s: %s",
                                  failed_atomic->id, error != nullptr ? error : "failed");
                    g_gate_degraded[slot.plugin_index] = true;
                    infra::status_set_degraded(plugin.id, true);
                    infra::status_update(plugin.id, infra::FeatureState::kInstalledDisabled,
                                         false, g_error_text[slot.plugin_index]);
                    QOL_LOG_WARN(QolDomain::kCore, "plugin %s atomic group deferred: %s", plugin.id,
                                 g_error_text[slot.plugin_index]);
                } else {
                    infra::status_update(plugin.id, infra::FeatureState::kFailed, false, error);
                    QOL_LOG_WARN(QolDomain::kCore, "plugin %s lifecycle failed: %s", plugin.id,
                                 error);
                }
            } else {
                infra::status_update(plugin.id, infra::FeatureState::kFailed, false, error);
                QOL_LOG_WARN(QolDomain::kCore, "plugin %s lifecycle failed: %s", plugin.id,
                             error);
            }
        }
        slot.in_use = false;
        slot.cascade = false;
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
        g_gate_degraded[i] = false;
    }
}

}  // namespace qol::host
