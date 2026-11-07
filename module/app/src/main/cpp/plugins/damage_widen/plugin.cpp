#include "plugin.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../../contracts/game_host.h"

namespace qol::features::damage_widen {
namespace {

constexpr const char* kPluginId = "damage_widen";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kResourceNormal = "hook:CHAR_GetNormalDamage";
constexpr const char* kResourceMagic = "hook:CHAR_GetNormalMagicDamage";
constexpr const char* kResourceCritical = "hook:CHAR_GetCriticalDamage";
constexpr const char* kResourceDefense = "hook:CHAR_CalcMagicalDefense";

using NormalFn = int32_t (*)(void*);
using CriticalFn = int32_t (*)(void*, void*, int32_t);
using DefenseFn = int32_t (*)(void*, void*, int32_t);

std::atomic<bool> g_enabled{false};
std::atomic<const contracts::QolHostSdk*> g_sdk{nullptr};
std::atomic<const contracts::QolCharacterReadApi*> g_character_read{nullptr};
std::atomic<NormalFn> g_normal_backup{nullptr};
std::atomic<NormalFn> g_magic_backup{nullptr};
std::atomic<CriticalFn> g_critical_backup{nullptr};
std::atomic<DefenseFn> g_defense_backup{nullptr};
std::atomic<bool> g_attr_warned[4]{};

// 日志经 contracts 面 QolHostSdk.log 输出（core.item inventory_logging 同模式），
// 插件不得 include infra/game 层日志头。域序数 16 = infra QolDomain::kWorldTuning（数值域）。
constexpr int32_t kLogDomainWorldTuning = 16;

const char* log_basename(const char* path) {
    if (path == nullptr) return "";
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

void log_write(int32_t level, const char* file, int line, const char* format, ...) {
    const auto* sdk = g_sdk.load(std::memory_order_acquire);
    if (sdk == nullptr || sdk->log.write == nullptr || file == nullptr || format == nullptr) {
        return;
    }
    std::array<char, contracts::kMessageCap + 1> message{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message.data(), message.size(), format, args);
    va_end(args);
    if (written < 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= contracts::kMessageCap
        ? contracts::kMessageCap
        : static_cast<uint32_t>(written);
    char source[160]{};
    std::snprintf(source, sizeof(source), "%s:%d", log_basename(file), line);
    (void)sdk->log.write(level, kLogDomainWorldTuning, source, message.data(), length);
}

int32_t clamp_damage_i64(int64_t value) {
    if (value < 0) return 0;
    if (value > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(value);
}

// 单点读取失败即整体降级：wrapper 透传 backup 原函数（原生数学），避免用残缺属性算出错误伤害。
// 首次失败按资源告警一次，防刷屏。
bool read_attr(void* character, int32_t attr_id, uint32_t resource_index, int32_t* out_value) {
    const auto* api = g_character_read.load(std::memory_order_acquire);
    if (api != nullptr && api->attr != nullptr &&
        contracts::succeeded(api->attr(character, attr_id, out_value))) {
        return true;
    }
    if (resource_index < 4 && !g_attr_warned[resource_index].exchange(true)) {
        log_write(2, __FILE__, __LINE__,
                  "damage_widen CHAR_GetAttr failed resource=%u attr=%d; fallback to original",
                  resource_index, attr_id);
    }
    return false;
}

int32_t get_normal_damage_wrapper(void* ch) {
    const auto* sdk = g_sdk.load(std::memory_order_acquire);
    const contracts::InFlightGuard guard(sdk == nullptr ? nullptr : &sdk->in_flight,
                                          {kResourceNormal});
    const auto backup = g_normal_backup.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_acquire) ||
        g_character_read.load(std::memory_order_acquire) == nullptr) {
        return backup == nullptr ? 0 : backup(ch);
    }
    int32_t atk = 0;
    int32_t bonus = 0;
    int32_t crit_gate = 0;
    int32_t crit_extra = 0;
    if (!read_attr(ch, 0x04, 0, &atk) || !read_attr(ch, 0x05, 0, &bonus) ||
        !read_attr(ch, 0x6a, 0, &crit_gate) || !read_attr(ch, 0x6d, 0, &crit_extra)) {
        return backup == nullptr ? 0 : backup(ch);
    }
    if (crit_gate > 0) bonus += crit_extra;
    return clamp_damage_i64(static_cast<int64_t>(bonus + 1000) *
                            static_cast<int64_t>(atk) / 1000);
}

int32_t get_normal_magic_damage_wrapper(void* ch) {
    const auto* sdk = g_sdk.load(std::memory_order_acquire);
    const contracts::InFlightGuard guard(sdk == nullptr ? nullptr : &sdk->in_flight,
                                          {kResourceMagic});
    const auto backup = g_magic_backup.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_acquire) ||
        g_character_read.load(std::memory_order_acquire) == nullptr) {
        return backup == nullptr ? 0 : backup(ch);
    }
    int32_t discarded = 0;
    int32_t matk = 0;
    int32_t rate = 0;
    // 原函数按 9,8,8,9 顺序读四次，前两次结果丢弃；保序保真。
    if (!read_attr(ch, 9, 1, &discarded) || !read_attr(ch, 8, 1, &discarded) ||
        !read_attr(ch, 8, 1, &matk) || !read_attr(ch, 9, 1, &rate)) {
        return backup == nullptr ? 0 : backup(ch);
    }
    return clamp_damage_i64(static_cast<int64_t>(rate + 1000) *
                            static_cast<int64_t>(matk) / 1000);
}

int32_t get_critical_damage_wrapper(void* unused, void* ch, int32_t damage) {
    const auto* sdk = g_sdk.load(std::memory_order_acquire);
    const contracts::InFlightGuard guard(sdk == nullptr ? nullptr : &sdk->in_flight,
                                          {kResourceCritical});
    const auto backup = g_critical_backup.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_acquire) ||
        g_character_read.load(std::memory_order_acquire) == nullptr) {
        return backup == nullptr ? 0 : backup(unused, ch, damage);
    }
    int32_t resist = 0;
    int32_t bonus_base = 0;
    if (!read_attr(ch, 2, 2, &resist) || !read_attr(ch, 3, 2, &bonus_base)) {
        return backup == nullptr ? 0 : backup(unused, ch, damage);
    }
    int32_t diff = 1000 - resist;
    if (diff < 0) diff = 0;  // 原函数 csel ge：暴抗超过 1000 时加成段钳 0
    const int64_t total = static_cast<int64_t>(bonus_base) + diff;
    const int64_t result = static_cast<int64_t>(damage) +
                           total * static_cast<int64_t>(damage) / 1000;
    return clamp_damage_i64(result);
}

int32_t calc_magical_defense_wrapper(void* unused, void* defender, int32_t damage) {
    const auto* sdk = g_sdk.load(std::memory_order_acquire);
    const contracts::InFlightGuard guard(sdk == nullptr ? nullptr : &sdk->in_flight,
                                          {kResourceDefense});
    const auto backup = g_defense_backup.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_acquire) ||
        g_character_read.load(std::memory_order_acquire) == nullptr) {
        return backup == nullptr ? 0 : backup(unused, defender, damage);
    }
    int32_t rate = 0;
    if (!read_attr(defender, 0x0b, 3, &rate)) {
        return backup == nullptr ? 0 : backup(unused, defender, damage);
    }
    return clamp_damage_i64((static_cast<int64_t>(1000) - rate) *
                            static_cast<int64_t>(damage) / 1000);
}

const contracts::ResourceClaim kResources[] = {
    {kResourceNormal, "F_CHAR_GET_NORMAL_DAMAGE_VMA", contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "千分比定点乘法 int32 回绕点：普攻伤害上限约 214.7 万，超限回绕为负值致崩溃",
     nullptr, 120, reinterpret_cast<void*>(&get_normal_damage_wrapper), true},
    {kResourceMagic, "F_CHAR_GET_NORMAL_MAGIC_DAMAGE_VMA", contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "魔攻千分比定点乘法在 int32 中间结果处回绕",
     nullptr, 120, reinterpret_cast<void*>(&get_normal_magic_damage_wrapper), true},
    {kResourceCritical, "F_CHAR_GET_CRITICAL_DAMAGE_VMA", contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "暴击增伤千分比定点乘法在 int32 中间结果处回绕",
     nullptr, 120, reinterpret_cast<void*>(&get_critical_damage_wrapper), true},
    {kResourceDefense, "F_CHAR_CALC_MAGICAL_DEFENSE_VMA", contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "魔防千分比定点乘法在 int32 中间结果处回绕",
     nullptr, 120, reinterpret_cast<void*>(&calc_magical_defense_wrapper), true},
};

bool character_read_available(const contracts::QolGameHost* game) {
    return game != nullptr && game->abi.size >= sizeof(contracts::QolGameHost) &&
           game->abi.major == contracts::kGameHostMajor &&
           game->abi.minor >= contracts::kGameHostCharacterReadMinor &&
           game->character_read.attr != nullptr;
}

int plugin_install(contracts::PluginContext& ctx) {
    g_enabled.store(false, std::memory_order_release);
    g_sdk.store(nullptr, std::memory_order_release);
    g_character_read.store(nullptr, std::memory_order_release);
    if (ctx.struct_size < sizeof(contracts::PluginContext) || ctx.sdk == nullptr ||
        ctx.game == nullptr) return 1;
    g_sdk.store(ctx.sdk, std::memory_order_release);
    if (character_read_available(ctx.game))
        g_character_read.store(&ctx.game->character_read, std::memory_order_release);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(false, std::memory_order_release);
    g_character_read.store(nullptr, std::memory_order_release);
    g_sdk.store(nullptr, std::memory_order_release);
}

void plugin_on_enabled(contracts::PluginContext&) { g_enabled.store(true, std::memory_order_release); }
void plugin_on_disabled(contracts::PluginContext&) { g_enabled.store(false, std::memory_order_release); }

int32_t on_resource_ready(contracts::PluginContext&, const char* resource_id, void* backup) {
    if (resource_id == nullptr || backup == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceNormal) == 0)
        g_normal_backup.store(reinterpret_cast<NormalFn>(backup), std::memory_order_release);
    else if (std::strcmp(resource_id, kResourceMagic) == 0)
        g_magic_backup.store(reinterpret_cast<NormalFn>(backup), std::memory_order_release);
    else if (std::strcmp(resource_id, kResourceCritical) == 0)
        g_critical_backup.store(reinterpret_cast<CriticalFn>(backup), std::memory_order_release);
    else if (std::strcmp(resource_id, kResourceDefense) == 0)
        g_defense_backup.store(reinterpret_cast<DefenseFn>(backup), std::memory_order_release);
    return 0;
}

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"damage_widen.enabled", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "数值守卫", "防止数值崩溃，扩大数值上限",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId, kPluginVersion, 0, nullptr, 0, nullptr, 0, nullptr,
    static_cast<uint32_t>(sizeof(kResources) / sizeof(kResources[0])), kResources,
    0, nullptr, &plugin_install, &plugin_deactivate, &plugin_on_enabled, &plugin_on_disabled,
    nullptr, &on_resource_ready, nullptr, 0, nullptr, 0, kFeatures, 1};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::damage_widen
