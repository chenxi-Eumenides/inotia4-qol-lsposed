#include "plugin.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../../contracts/game_ports/combat_move_port.h"
#include "../../contracts/game_ports/ports.h"

namespace qol::features::combat_move {
namespace {

constexpr const char* kPluginId = "combat_move";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kKeepTargetResource = "patch:GAMESTATE_PressKeyPlay+0x1d4";
constexpr const char* kMoveGateResource = "patch:CHAR_Move+0x40";
constexpr const char* kSetActionResource = "hook:CHAR_SetAction";
constexpr const char* kStartActionExResource = "hook:CHAR_StartActionEx";

using SetActionFn = void (*)(void*, void*, void*);
using StartActionExFn = int32_t (*)(void*, void*, int32_t);

std::atomic<bool> g_enabled{false};
std::atomic<const contracts::QolHostSdk*> g_sdk{nullptr};
std::atomic<const contracts::QolCombatMovePort*> g_port{nullptr};
std::atomic<SetActionFn> g_set_action_backup{nullptr};
std::atomic<StartActionExFn> g_start_action_ex_backup{nullptr};

// 范围余量随攻击距离递减：距离 4 时 +100%，距离 13 时 +10%，中间线性，越界按端点夹取。
// 基础攻击动作的距离取值只有 4/6/12/13（ACTDATABASE +0x05，与 CHAR_GetDistance 同口径）。
constexpr int32_t kMarginNearRange = 4;
constexpr int32_t kMarginFarRange = 13;
constexpr int32_t kMarginMaxPct = 100;
constexpr int32_t kMarginMinPct = 10;

int32_t margin_pct(int32_t range) {
    const int32_t pct = kMarginMinPct + (kMarginFarRange - range) * 10;
    if (pct > kMarginMaxPct) return kMarginMaxPct;
    if (pct < kMarginMinPct) return kMarginMinPct;
    return pct;
}

bool target_locked(const contracts::QolCombatMovePort* port, void* character) {
    if (!g_enabled.load(std::memory_order_acquire) || port == nullptr || character == nullptr ||
        port->is_active_player == nullptr || port->is_player_side == nullptr ||
        port->target == nullptr) {
        return false;
    }
    return port->is_active_player(character) != 0 && port->is_player_side(character) != 0 &&
           port->target(character) != nullptr;
}

bool attacking(const contracts::QolCombatMovePort* port, void* character) {
    if (!target_locked(port, character) || port->current_action == nullptr ||
        port->is_default_attack == nullptr) {
        return false;
    }
    const void* action = port->current_action(character);
    return action != nullptr && port->is_default_attack(character, action) != 0;
}

contracts::InFlightGuard resource_guard(const char* resource_id) {
    const contracts::QolHostSdk* sdk = g_sdk.load(std::memory_order_acquire);
    return contracts::InFlightGuard(sdk == nullptr ? nullptr : &sdk->in_flight, {resource_id});
}

QolResult toggle(contracts::PluginContext&, uint8_t enabled) {
    g_enabled.store(enabled != 0, std::memory_order_release);
    return contracts::ok();
}

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_COMBAT_MOVE,
     {contracts::kQolPortCombatMoveVersion.major, contracts::kQolPortCombatMoveVersion.minor}},
};

const contracts::ResourceClaim kResources[] = {
    {kKeepTargetResource, "P_GAMESTATE_PRESSKEYPLAY_KEEP_TARGET",
     contracts::ResourceMechanism::kCallPatch, contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired, contracts::ResourceFailure::kFailClosed, true,
     kPluginId, "攻击移动态保留已锁定目标", nullptr, 120,
      reinterpret_cast<void*>(&keep_target_wrapper), true},
    {kMoveGateResource, "P_CHAR_MOVE_IS_NO_MOVE", contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailClosed, true, kPluginId,
      "攻击移动态允许移动继续", nullptr, 120, reinterpret_cast<void*>(&move_gate_wrapper), true},
    {kSetActionResource, "F_CHAR_SET_ACTION_VMA", contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailClosed, true, kPluginId,
     "攻击移动态抑制走路动作切换", nullptr, 120,
      reinterpret_cast<void*>(&set_action_wrapper), true},
    {kStartActionExResource, "F_CHAR_START_ACTION_EX_VMA",
     contracts::ResourceMechanism::kEntryHook, contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired, contracts::ResourceFailure::kFailClosed, true,
     kPluginId, "普攻出手前检查攻击距离", nullptr, 120,
      reinterpret_cast<void*>(&start_action_ex_wrapper), true},
};
static_assert(kResourceCount == (sizeof(kResources) / sizeof(kResources[0])),
              "kResourceCount must match kResources");

int plugin_install(contracts::PluginContext& ctx) {
    g_enabled.store(false, std::memory_order_release);
    g_port.store(nullptr, std::memory_order_release);
    g_sdk.store(nullptr, std::memory_order_release);
    g_set_action_backup.store(nullptr, std::memory_order_release);
    g_start_action_ex_backup.store(nullptr, std::memory_order_release);
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr ||
        ctx.game == nullptr || ctx.ports == nullptr || ctx.port_count != 1 ||
        ctx.ports[0] == nullptr) {
        return 1;
    }
    const auto* port = static_cast<const contracts::QolCombatMovePort*>(ctx.ports[0]);
    if (port->abi_version.major != contracts::kQolPortCombatMoveVersion.major ||
        port->is_active_player == nullptr || port->is_player_side == nullptr ||
        port->target == nullptr || port->current_action == nullptr ||
        port->is_default_attack == nullptr || port->is_walk_action == nullptr ||
        port->distance == nullptr || port->action_range == nullptr ||
        port->is_no_move == nullptr || port->set_target == nullptr) {
        return 1;
    }
    g_sdk.store(ctx.sdk, std::memory_order_release);
    g_port.store(port, std::memory_order_release);
    return 0;
}

void plugin_deactivate(contracts::PluginContext&) {
    g_enabled.store(false, std::memory_order_release);
    g_port.store(nullptr, std::memory_order_release);
    g_sdk.store(nullptr, std::memory_order_release);
    g_set_action_backup.store(nullptr, std::memory_order_release);
    g_start_action_ex_backup.store(nullptr, std::memory_order_release);
}

void plugin_on_enabled(contracts::PluginContext&) {
    g_enabled.store(true, std::memory_order_release);
}

void plugin_on_disabled(contracts::PluginContext&) {
    g_enabled.store(false, std::memory_order_release);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_enabled.load(std::memory_order_acquire);
    out.error = nullptr;
}

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"combat_move.enabled", 0, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kToggle, "战斗优化", "移动不会打断攻击动作", &toggle, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId, kPluginVersion, 0, nullptr, 0, nullptr, 0, nullptr,
    static_cast<uint32_t>(sizeof(kResources) / sizeof(kResources[0])), kResources,
    0, nullptr, &plugin_install, &plugin_deactivate, &plugin_on_enabled, &plugin_on_disabled,
    &plugin_fill_status, &on_resource_ready, kPorts, 1, nullptr, 0, kFeatures, 1};

}  // namespace

int32_t effective_range(int32_t range) {
    if (range <= 0) return range;
    return range + (range * margin_pct(range)) / 100;
}

void keep_target_wrapper(void* character, void* target) {
    const auto guard = resource_guard(kKeepTargetResource);
    (void)guard;
    const auto* port = g_port.load(std::memory_order_acquire);
    if (target == nullptr && target_locked(port, character)) return;
    if (port != nullptr && port->set_target != nullptr) port->set_target(character, target);
}

int32_t move_gate_wrapper(void* character) {
    const auto guard = resource_guard(kMoveGateResource);
    (void)guard;
    const auto* port = g_port.load(std::memory_order_acquire);
    if (port == nullptr || port->is_no_move == nullptr) return 1;
    if (attacking(port, character)) return 0;
    return port->is_no_move(character);
}

void set_action_wrapper(void* character, void* action, void* target) {
    const auto guard = resource_guard(kSetActionResource);
    (void)guard;
    const auto* port = g_port.load(std::memory_order_acquire);
    if (attacking(port, character) && port->is_walk_action(character, action) != 0) return;
    const auto backup = g_set_action_backup.load(std::memory_order_acquire);
    if (backup != nullptr) backup(character, action, target);
}

int32_t start_action_ex_wrapper(void* character, void* action, int32_t flag) {
    const auto guard = resource_guard(kStartActionExResource);
    (void)guard;
    const auto* port = g_port.load(std::memory_order_acquire);
    const auto backup = g_start_action_ex_backup.load(std::memory_order_acquire);
    if (backup == nullptr) return 0;
    if (!target_locked(port, character) || action == nullptr ||
        port->is_default_attack(character, action) == 0) {
        return backup(character, action, flag);
    }
    void* target = port->target(character);
    const int32_t range = port->action_range(character, action);
    if (target == nullptr || port->distance(character, target) <= effective_range(range)) {
        return backup(character, action, flag);
    }
    // 超出范围只取消本次出手，保留目标；回到范围内后由动作链再次尝试。
    return 0;
}

int32_t on_resource_ready(contracts::PluginContext&, const char* resource_id, void* backup) {
    if (resource_id == nullptr || backup == nullptr) return 0;
    if (std::strcmp(resource_id, kSetActionResource) == 0) {
        g_set_action_backup.store(reinterpret_cast<SetActionFn>(backup), std::memory_order_release);
        return 0;
    }
    if (std::strcmp(resource_id, kStartActionExResource) == 0) {
        g_start_action_ex_backup.store(reinterpret_cast<StartActionExFn>(backup),
                                       std::memory_order_release);
        return 0;
    }
    return 0;
}

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::combat_move
