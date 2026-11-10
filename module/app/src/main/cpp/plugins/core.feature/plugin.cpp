#include "plugin.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "contracts/game_host.h"
#include "contracts/http.h"

namespace qol::features::core_feature {
namespace {

// This migration-only plugin temporarily owns confirmed unimplemented HTTP endpoints.
// Implemented endpoints must move to their domain owners, after which this plugin is removed.
constexpr const char* kPluginId = "core.feature";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kSkillUsagePath = "/api/character/combat/{role}/set_skill_usage";
constexpr char kNotImplementedBody[] = "{\"ok\":false,\"error\":\"not implemented\"}";
constexpr contracts::CapabilityRequirement kOptionalHttpRequires[] = {
    {contracts::kHttpRegistryCapabilityId,
     {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId,
     {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor}},
};

// 暴击分支的受击闪白 0x5a 与横向蓝白打击条 0x1f..0x22：
// 两者都只在 CHAR_AddDamage 的暴击分支添加，与暴击光效 0x52/0x1df 分开。
constexpr int32_t kCriticalFlashEffectId = 0x5a;
constexpr int32_t kCriticalBarEffectMin = 0x1f;
constexpr int32_t kCriticalBarEffectMax = 0x22;

bool is_masked_crit_effect(int32_t effect_id) {
    return effect_id == kCriticalFlashEffectId ||
           (effect_id >= kCriticalBarEffectMin && effect_id <= kCriticalBarEffectMax);
}

constexpr const char* kResourceAddActEffect = "hook:CHAR_AddActEffect";
constexpr const char* kResourceMapSetShake = "hook:MAP_SetShake";
constexpr const char* kResourceVibrationPlay = "hook:VIBRATION_Play";

using AddActEffectFn = int32_t (*)(void*, void*, int32_t);
using MapSetShakeFn = void (*)(int32_t);
using VibrationPlayFn = void (*)(int32_t);
using FrameCountFn = int64_t (*)();

const contracts::QolHostSdk* g_sdk = nullptr;

std::atomic<bool> g_mask_crit_effects{true};
std::atomic<AddActEffectFn> g_add_act_effect_backup{nullptr};
std::atomic<MapSetShakeFn> g_map_set_shake_backup{nullptr};
std::atomic<VibrationPlayFn> g_vibration_play_backup{nullptr};
std::atomic<FrameCountFn> g_frame_count{nullptr};

// 暴击分支顺序固定为：0x52/0x1df 光效 -> 0x5a 闪白 -> 随机打击 -> 震屏 -> 震动。
// 只拦截同一帧内紧跟 0x5a 的震屏/震动，避免影响技能和其他来源的反馈。
std::atomic<bool> g_crit_feedback_pending{false};
std::atomic<int64_t> g_crit_feedback_frame{-1};

int64_t current_frame() {
    const auto frame_count = g_frame_count.load(std::memory_order_acquire);
    return frame_count == nullptr ? -1 : frame_count();
}

bool crit_feedback_pending() {
    if (!g_crit_feedback_pending.load(std::memory_order_acquire)) return false;
    const auto frame_count = g_frame_count.load(std::memory_order_acquire);
    if (frame_count != nullptr) {
        const int64_t frame = frame_count();
        const int64_t expected = g_crit_feedback_frame.load(std::memory_order_acquire);
        if (expected >= 0 && frame != expected) {
            g_crit_feedback_pending.store(false, std::memory_order_release);
            return false;
        }
    }
    return true;
}

int32_t add_act_effect_wrapper(void* attacker, void* target, int32_t effect_id) {
    const auto backup = g_add_act_effect_backup.load(std::memory_order_acquire);
    if (g_mask_crit_effects.load(std::memory_order_acquire) && is_masked_crit_effect(effect_id)) {
        if (effect_id == kCriticalFlashEffectId) {
            g_crit_feedback_frame.store(current_frame(), std::memory_order_release);
            g_crit_feedback_pending.store(true, std::memory_order_release);
        }
        return 0;
    }
    return backup == nullptr ? 0 : backup(attacker, target, effect_id);
}

void map_set_shake_wrapper(int32_t type) {
    const auto backup = g_map_set_shake_backup.load(std::memory_order_acquire);
    if (g_mask_crit_effects.load(std::memory_order_acquire) && crit_feedback_pending()) {
        // 保留 pending，紧随其后的 VIBRATION_Play 也要一起拦截。
        return;
    }
    if (backup != nullptr) backup(type);
}

void vibration_play_wrapper(int32_t type) {
    const auto backup = g_vibration_play_backup.load(std::memory_order_acquire);
    if (g_mask_crit_effects.load(std::memory_order_acquire) && crit_feedback_pending()) {
        g_crit_feedback_pending.store(false, std::memory_order_release);
        return;
    }
    if (backup != nullptr) backup(type);
}

QolResult skill_usage_command(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < sizeof(kNotImplementedBody) - 1) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, kNotImplementedBody, sizeof(kNotImplementedBody) - 1);
    response->body_size = sizeof(kNotImplementedBody) - 1;
    response->status = 501;
    return contracts::ok();
}

const contracts::HttpEndpointDescriptor kHttpEndpoints[] = {
    {contracts::HttpMethod::kPost, kSkillUsagePath, kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &skill_usage_command, nullptr},
};

const contracts::ResourceClaim kResources[] = {
    {kResourceAddActEffect, "F_CHAR_ADD_ACT_EFFECT_VMA",
     contracts::ResourceMechanism::kEntryHook, contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional, contracts::ResourceFailure::kFailSoft, true,
     kPluginId, "暴击闪白 0x5a 与横向打击条 0x1f..0x22 的唯一入口；按特效 ID 拦截，其余特效透传",
     nullptr, 120, reinterpret_cast<void*>(&add_act_effect_wrapper)},
    {kResourceMapSetShake, "F_MAP_SET_SHAKE_VMA",
     contracts::ResourceMechanism::kEntryHook, contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional, contracts::ResourceFailure::kFailSoft, true,
     kPluginId, "暴击震屏与闪白同帧出现；仅拦截暴击分支触发的震屏",
     nullptr, 120, reinterpret_cast<void*>(&map_set_shake_wrapper)},
    {kResourceVibrationPlay, "F_VIBRATION_PLAY_VMA",
     contracts::ResourceMechanism::kEntryHook, contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional, contracts::ResourceFailure::kFailSoft, true,
     kPluginId, "暴击震动与闪白同帧出现；仅拦截暴击分支触发的震动",
     nullptr, 120, reinterpret_cast<void*>(&vibration_play_wrapper)},
};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr) return 1;
    g_sdk = ctx.sdk;
    g_mask_crit_effects.store(true, std::memory_order_release);
    g_add_act_effect_backup.store(nullptr, std::memory_order_release);
    g_map_set_shake_backup.store(nullptr, std::memory_order_release);
    g_vibration_play_backup.store(nullptr, std::memory_order_release);
    g_frame_count.store(
        ctx.game != nullptr && ctx.game->thread.frame_count != nullptr
            ? ctx.game->thread.frame_count
            : nullptr,
        std::memory_order_release);
    g_crit_feedback_pending.store(false, std::memory_order_release);
    g_crit_feedback_frame.store(-1, std::memory_order_release);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_sdk = nullptr;
    g_mask_crit_effects.store(false, std::memory_order_release);
    g_add_act_effect_backup.store(nullptr, std::memory_order_release);
    g_map_set_shake_backup.store(nullptr, std::memory_order_release);
    g_vibration_play_backup.store(nullptr, std::memory_order_release);
    g_frame_count.store(nullptr, std::memory_order_release);
    g_crit_feedback_pending.store(false, std::memory_order_release);
    g_crit_feedback_frame.store(-1, std::memory_order_release);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_sdk != nullptr;
    out.error = nullptr;
}

int32_t on_resource_ready(contracts::PluginContext&, const char* resource_id, void* backup) {
    if (resource_id == nullptr || backup == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceAddActEffect) == 0) {
        g_add_act_effect_backup.store(reinterpret_cast<AddActEffectFn>(backup),
                                      std::memory_order_release);
    } else if (std::strcmp(resource_id, kResourceMapSetShake) == 0) {
        g_map_set_shake_backup.store(reinterpret_cast<MapSetShakeFn>(backup),
                                     std::memory_order_release);
    } else if (std::strcmp(resource_id, kResourceVibrationPlay) == 0) {
        g_vibration_play_backup.store(reinterpret_cast<VibrationPlayFn>(backup),
                                      std::memory_order_release);
    }
    return 0;
}

QolResult mask_crit_effects_toggle(contracts::PluginContext&, uint8_t enabled) {
    g_mask_crit_effects.store(enabled != 0, std::memory_order_release);
    if (enabled == 0) {
        g_crit_feedback_pending.store(false, std::memory_order_release);
        g_crit_feedback_frame.store(-1, std::memory_order_release);
    }
    return contracts::ok();
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"core.feature.compatibility_routes", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "迁移占位路由", "",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
    {"core.feature.mask_crit_effects", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "屏蔽暴击特效",
     "拦截暴击闪白、横向打击条、震屏与震动，保留暴击光效", &mask_crit_effects_toggle, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    2,
    kOptionalHttpRequires,
    0,
    nullptr,
    static_cast<uint32_t>(sizeof(kResources) / sizeof(kResources[0])),
    kResources,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    nullptr,
    nullptr,
    &plugin_fill_status,
    &on_resource_ready,
    nullptr, 0,
    nullptr, 0,
    kFeatures, 2,
    kHttpEndpoints, 1,
    nullptr,
    nullptr, 0,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::core_feature
