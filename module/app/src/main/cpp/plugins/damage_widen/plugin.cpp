#include "plugin.h"
#include "feature.h"
#include "safe_scale.h"

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
constexpr const char* kResourceActDdEntry = "hook:CHAR_Act_DD";
constexpr const char* kResourcePhysical = kResourcePhysicalDefense;

using NormalFn = int32_t (*)(void*);
using CriticalFn = int32_t (*)(void*, void*, int32_t);
using DefenseFn = int32_t (*)(void*, void*, int32_t);
using ActDdFn = int32_t (*)(void*, void*, int32_t, int32_t);

std::atomic<bool> g_enabled{false};
std::atomic<const contracts::QolHostSdk*> g_sdk{nullptr};
std::atomic<const contracts::QolCharacterReadApi*> g_character_read{nullptr};
std::atomic<NormalFn> g_normal_backup{nullptr};
std::atomic<NormalFn> g_magic_backup{nullptr};
std::atomic<CriticalFn> g_critical_backup{nullptr};
std::atomic<DefenseFn> g_defense_backup{nullptr};
std::atomic<DefenseFn> g_physical_backup{nullptr};
std::atomic<ActDdFn> g_act_dd_backup{nullptr};
std::atomic<bool> g_attr_warned[5]{};
std::atomic<uint32_t> g_normal_trace_count{0};
std::atomic<uint32_t> g_critical_trace_count{0};
std::atomic<uint32_t> g_physical_trace_count{0};
std::atomic<uint32_t> g_physical_caller_trace_count{0};
std::atomic<uint32_t> g_act_dd_trace_count{0};

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

bool trace_stage_available(std::atomic<uint32_t>& count) {
    uint32_t current = count.load(std::memory_order_relaxed);
    while (current < 12) {
        if (count.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
            return true;
    }
    return false;
}

// 单点读取失败即整体降级：wrapper 透传 backup 原函数（原生数学），避免用残缺属性算出错误伤害。
// 首次失败按资源告警一次，防刷屏。
bool read_attr(void* character, int32_t attr_id, uint32_t resource_index, int32_t* out_value) {
    const auto* api = g_character_read.load(std::memory_order_acquire);
    if (api != nullptr && api->attr != nullptr &&
        contracts::succeeded(api->attr(character, attr_id, out_value))) {
        return true;
    }
    if (resource_index < 5 && !g_attr_warned[resource_index].exchange(true)) {
        log_write(2, __FILE__, __LINE__,
                  "damage_widen CHAR_GetAttr failed resource=%u attr=%d; fallback to original",
                  resource_index, attr_id);
    }
    return false;
}

int32_t get_normal_damage_wrapper(void* ch) {
    const void* caller = __builtin_return_address(0);
    const auto* sdk = g_sdk.load(std::memory_order_acquire);
    const contracts::InFlightGuard guard(sdk == nullptr ? nullptr : &sdk->in_flight,
                                          {kResourceNormal});
    const auto backup = g_normal_backup.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_acquire) ||
        g_character_read.load(std::memory_order_acquire) == nullptr) {
        return backup == nullptr ? 0 : backup(ch);
    }
    int32_t atk = 0;
    int32_t bonus_base = 0;
    int32_t crit_gate = 0;
    int32_t crit_extra = 0;
    if (!read_attr(ch, 0x04, 0, &atk) || !read_attr(ch, 0x05, 0, &bonus_base) ||
        !read_attr(ch, 0x6a, 0, &crit_gate) || !read_attr(ch, 0x6d, 0, &crit_extra)) {
        return backup == nullptr ? 0 : backup(ch);
    }
    const int64_t bonus = static_cast<int64_t>(bonus_base) +
                          (crit_gate > 0 ? crit_extra : 0);
    const int32_t result = clamp_damage_i64(safe_scale(atk, bonus + 1000));
    if (trace_stage_available(g_normal_trace_count)) {
        log_write(1, __FILE__, __LINE__,
                  "[damage_trace] stage=normal character=%p caller=%p attr4=%d attr5=%d attr106=%d attr109=%d base_damage=%d",
                  ch, caller, atk, bonus_base, crit_gate, crit_extra, result);
    }
    return result;
}

int32_t act_dd_entry_wrapper(void* attacker, void* victim, int32_t w2, int32_t action_type) {
    const auto backup = g_act_dd_backup.load(std::memory_order_acquire);
    if (backup == nullptr) return 0;
    if (trace_stage_available(g_act_dd_trace_count)) {
        const void* caller = __builtin_return_address(0);
        int32_t attr5 = 0, attr6 = 0, attr71 = 0, attr81 = 0, attr106 = 0, attr109 = 0;
        uint32_t attr_mask = 0;
        const auto* api = g_character_read.load(std::memory_order_acquire);
        if (api != nullptr && api->attr != nullptr && attacker != nullptr) {
            const auto read_trace_attr = [&](int32_t id, int32_t* value, uint32_t bit) {
                if (contracts::succeeded(api->attr(attacker, id, value))) attr_mask |= bit;
            };
            read_trace_attr(5, &attr5, 1u << 0);
            read_trace_attr(6, &attr6, 1u << 1);
            read_trace_attr(71, &attr71, 1u << 2);
            read_trace_attr(81, &attr81, 1u << 3);
            read_trace_attr(106, &attr106, 1u << 4);
            read_trace_attr(109, &attr109, 1u << 5);
        }
        log_write(1, __FILE__, __LINE__,
                  "[damage_trace] stage=act_dd_entry attacker=%p victim=%p w2=%d action_type=%d caller=%p attrs_mask=%u attr5=%d attr6=%d attr71=%d attr81=%d attr106=%d attr109=%d",
                  attacker, victim, w2, action_type, caller, attr_mask, attr5, attr6, attr71,
                  attr81, attr106, attr109);
    }
    return backup(attacker, victim, w2, action_type);
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
    return clamp_damage_i64(safe_scale(matk, static_cast<int64_t>(rate) + 1000));
}

int32_t get_critical_damage_wrapper(void* attacker, void* defender, int32_t damage) {
    const void* caller = __builtin_return_address(0);
    const auto* sdk = g_sdk.load(std::memory_order_acquire);
    const contracts::InFlightGuard guard(sdk == nullptr ? nullptr : &sdk->in_flight,
                                          {kResourceCritical});
    const auto backup = g_critical_backup.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_acquire) ||
        g_character_read.load(std::memory_order_acquire) == nullptr) {
        return backup == nullptr ? 0 : backup(attacker, defender, damage);
    }
    int32_t resist = 0;
    int32_t bonus_base = 0;
    // 原版 CHAR_GetCriticalDamage：attr2（暴击抵抗）取自第二参数 defender，
    // attr3（暴击增伤）取自第一参数 attacker；两者不能混用同一角色。
    if (!read_attr(defender, 2, 2, &resist) || !read_attr(attacker, 3, 2, &bonus_base)) {
        return backup == nullptr ? 0 : backup(attacker, defender, damage);
    }
    int32_t diff = 1000 - resist;
    if (diff < 0) diff = 0;  // 原函数 csel ge：暴抗超过 1000 时加成段钳 0
    const int64_t total = static_cast<int64_t>(bonus_base) + diff;
    const int64_t result = static_cast<int64_t>(damage) + safe_scale(damage, total);
    const int32_t output = clamp_damage_i64(result);
    if (trace_stage_available(g_critical_trace_count)) {
        log_write(1, __FILE__, __LINE__,
                  "[damage_trace] stage=critical attacker=%p defender=%p caller=%p damage_in=%d attr2=%d attr3=%d damage_out=%d",
                  attacker, defender, caller, damage, resist, bonus_base, output);
    }
    return output;
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
    return clamp_damage_i64(safe_scale(static_cast<int64_t>(1000) - rate, damage));
}

int32_t clamp_signed_i32(int64_t value) {
    if (value < std::numeric_limits<int32_t>::min()) return std::numeric_limits<int32_t>::min();
    if (value > std::numeric_limits<int32_t>::max()) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(value);
}

int32_t physical_defense_fallback(void* unused, void* defender, int32_t damage,
                                  DefenseFn backup, const char* reason, const void* caller) {
    const int32_t output = backup == nullptr ? 0 : backup(unused, defender, damage);
    if (trace_stage_available(g_physical_trace_count)) {
        log_write(1, __FILE__, __LINE__,
                  "[damage_trace] stage=physical_defense defender=%p caller=%p damage_in=%d fallback=%s damage_out=%d",
                  defender, caller, damage, reason, output);
    }
    return output;
}

int32_t calc_physical_defense_wrapper(void* unused, void* defender, int32_t damage) {
    int32_t caller_w22 = 0;
    int32_t caller_w27 = 0;
#if defined(__aarch64__)
    asm volatile("str w22, %0\n\tstr w27, %1"
                 : "=m"(caller_w22), "=m"(caller_w27)
                 :
                 : "memory");
#endif
    const void* caller = __builtin_return_address(0);
    if (trace_stage_available(g_physical_caller_trace_count)) {
        log_write(1, __FILE__, __LINE__,
                  "[damage_trace] stage=physical_caller defender=%p caller=%p damage_arg=%d caller_w22=%d caller_w27=%d",
                  defender, caller, damage, caller_w22, caller_w27);
    }
    const auto* sdk = g_sdk.load(std::memory_order_acquire);
    const contracts::InFlightGuard guard(sdk == nullptr ? nullptr : &sdk->in_flight,
                                          {kResourcePhysical});
    const auto backup = g_physical_backup.load(std::memory_order_acquire);
    const auto* api = g_character_read.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_acquire) || api == nullptr ||
        defender == nullptr) {
        return physical_defense_fallback(unused, defender, damage, backup,
                                         "inactive_or_missing_api", caller);
    }

    int32_t attr17 = 0, attr18 = 0, attr7 = 0, attr81 = 0;
    if (!read_attr(defender, 17, 4, &attr17) || !read_attr(defender, 18, 4, &attr18))
        return physical_defense_fallback(unused, defender, damage, backup,
                                         "attr17_or_18", caller);
    const int64_t scaled = safe_scale(attr17, static_cast<int64_t>(attr18) + 1000);
    const int32_t cal_arg = clamp_signed_i32(scaled);
    int32_t cal_result = 0;
    if (api->physical_defense_calculate == nullptr ||
        !contracts::succeeded(api->physical_defense_calculate(defender, cal_arg, &cal_result)))
        return physical_defense_fallback(unused, defender, damage, backup,
                                         "cal_failed", caller);
    if (!read_attr(defender, 7, 4, &attr7) || !read_attr(defender, 81, 4, &attr81))
        return physical_defense_fallback(unused, defender, damage, backup,
                                         "attr7_or_81", caller);
    int64_t rate = static_cast<int64_t>(cal_result) + attr7;
    if (attr81 > 0) {
        const int64_t adjusted = safe_scale(rate, attr81);
        if (adjusted > 0 && rate > std::numeric_limits<int64_t>::max() - adjusted)
            rate = std::numeric_limits<int64_t>::max();
        else if (adjusted < 0 && rate < std::numeric_limits<int64_t>::min() - adjusted)
            rate = std::numeric_limits<int64_t>::min();
        else {
            rate += adjusted;
        }
    }
    if (damage == 0) {
        if (trace_stage_available(g_physical_trace_count)) {
            log_write(1, __FILE__, __LINE__,
                      "[damage_trace] stage=physical_defense defender=%p caller=%p damage_in=%d attr17=%d attr18=%d cal_arg=%d cal_result=%d attr7=%d attr81=%d rate=%lld damage_out=0",
                      defender, caller, damage, attr17, attr18, cal_arg, cal_result, attr7, attr81,
                      static_cast<long long>(rate));
        }
        return 0;
    }
    const int64_t magnitude = damage < 0 ? -static_cast<int64_t>(damage) : damage;
    const int64_t threshold = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) * 1000 /
                              magnitude;
    if (damage > 0) {
        if (rate >= 1000) {
            if (trace_stage_available(g_physical_trace_count)) {
                log_write(1, __FILE__, __LINE__,
                          "[damage_trace] stage=physical_defense defender=%p caller=%p damage_in=%d attr17=%d attr18=%d cal_arg=%d cal_result=%d attr7=%d attr81=%d rate=%lld damage_out=0",
                          defender, caller, damage, attr17, attr18, cal_arg, cal_result, attr7, attr81,
                          static_cast<long long>(rate));
            }
            return 0;
        }
        if (rate < 1000 - threshold) {
            const int32_t output = std::numeric_limits<int32_t>::max();
            if (trace_stage_available(g_physical_trace_count)) {
                log_write(1, __FILE__, __LINE__,
                          "[damage_trace] stage=physical_defense defender=%p caller=%p damage_in=%d attr17=%d attr18=%d cal_arg=%d cal_result=%d attr7=%d attr81=%d rate=%lld damage_out=%d",
                          defender, caller, damage, attr17, attr18, cal_arg, cal_result, attr7, attr81,
                          static_cast<long long>(rate), output);
            }
            return output;
        }
    } else {
        if (rate <= 1000) {
            if (trace_stage_available(g_physical_trace_count)) {
                log_write(1, __FILE__, __LINE__,
                          "[damage_trace] stage=physical_defense defender=%p caller=%p damage_in=%d attr17=%d attr18=%d cal_arg=%d cal_result=%d attr7=%d attr81=%d rate=%lld damage_out=0",
                          defender, caller, damage, attr17, attr18, cal_arg, cal_result, attr7, attr81,
                          static_cast<long long>(rate));
            }
            return 0;
        }
        if (rate > 1000 + threshold) {
            const int32_t output = std::numeric_limits<int32_t>::max();
            if (trace_stage_available(g_physical_trace_count)) {
                log_write(1, __FILE__, __LINE__,
                          "[damage_trace] stage=physical_defense defender=%p caller=%p damage_in=%d attr17=%d attr18=%d cal_arg=%d cal_result=%d attr7=%d attr81=%d rate=%lld damage_out=%d",
                          defender, caller, damage, attr17, attr18, cal_arg, cal_result, attr7, attr81,
                          static_cast<long long>(rate), output);
            }
            return output;
        }
    }
    const int32_t output = clamp_damage_i64(safe_scale(static_cast<int64_t>(1000) - rate, damage));
    if (trace_stage_available(g_physical_trace_count)) {
        log_write(1, __FILE__, __LINE__,
                  "[damage_trace] stage=physical_defense defender=%p caller=%p damage_in=%d attr17=%d attr18=%d cal_arg=%d cal_result=%d attr7=%d attr81=%d rate=%lld damage_out=%d",
                  defender, caller, damage, attr17, attr18, cal_arg, cal_result, attr7, attr81,
                  static_cast<long long>(rate), output);
    }
    return output;
}

const contracts::ResourceClaim kResources[] = {
    {kResourceActDdEntry, "F_CHAR_ACT_DD_VMA", contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_DD 入口参数诊断透传", nullptr, 120,
     reinterpret_cast<void*>(&act_dd_entry_wrapper), true},
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
    {kResourceWrapA, "INSN_ACT_M_DD_WRAP_A", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_M_DD 的怪物伤害千分比乘法使用 int64 并钳制",
     nullptr, 120, nullptr, false},
    {kResourceWrapB, "INSN_ACT_M_DD_WRAP_B", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_M_DD 的乘积保存在 64 位寄存器中",
     nullptr, 120, nullptr, false},
    {kResourceWrapBC, "INSN_ACT_M_DD_WRAP_BC", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_M_DD 的暴击增伤两级千分比乘除链使用 int64 并钳制",
      nullptr, 120, nullptr, false},
    {kResourceActEf20c, "INSN_ACT_DD_EF20C", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_DD 0xef20c 区域级有符号乘除钳制", nullptr, 120, nullptr, false},
    {kResourceActEf4d0, "INSN_ACT_DD_EF4D0", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_DD 0xef4d0 区域级有符号乘除钳制", nullptr, 120, nullptr, false},
    {kResourceActEf530, "INSN_ACT_DD_EF530", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_DD 0xef530 区域级有符号乘除钳制", nullptr, 120, nullptr, false},
    {kResourceActEf560, "INSN_ACT_DD_EF560", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_DD 0xef560 区域级有符号乘除钳制", nullptr, 120, nullptr, false},
    {kResourceActEf5d8, "INSN_ACT_DD_EF5D8", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_Act_DD 0xef5d8 区域级有符号乘除钳制", nullptr, 120, nullptr, false},
    {kResourceAttrAttackEb1b4, "INSN_ATTR_ATTACK_EB1B4", contracts::ResourceMechanism::kInsnPatch,
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft, true, kPluginId,
     "CHAR_AttrAttackDamage 0xeb1b4 区域级有符号乘除钳制", nullptr, 120, nullptr, false},
    {kResourcePhysical, "F_CHAR_CALC_PHYSICAL_DEFENSE_VMA",
     contracts::ResourceMechanism::kEntryHook, contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional, contracts::ResourceFailure::kFailSoft,
     true, kPluginId, "物理防御保留 CAL 文本公式并以宽中间值计算", nullptr, 120,
     reinterpret_cast<void*>(&calc_physical_defense_wrapper), true},
};

bool character_read_available(const contracts::QolGameHost* game) {
    return game != nullptr && game->abi.size >= sizeof(contracts::QolGameHost) &&
           game->abi.major == contracts::kGameHostMajor &&
           game->abi.minor >= contracts::kGameHostCharacterReadMinor &&
           game->character_read.attr != nullptr &&
           game->character_read.physical_defense_calculate != nullptr;
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
    if (std::strcmp(resource_id, kResourceActDdEntry) == 0)
        g_act_dd_backup.store(reinterpret_cast<ActDdFn>(backup), std::memory_order_release);
    else if (std::strcmp(resource_id, kResourceNormal) == 0)
        g_normal_backup.store(reinterpret_cast<NormalFn>(backup), std::memory_order_release);
    else if (std::strcmp(resource_id, kResourceMagic) == 0)
        g_magic_backup.store(reinterpret_cast<NormalFn>(backup), std::memory_order_release);
    else if (std::strcmp(resource_id, kResourceCritical) == 0)
        g_critical_backup.store(reinterpret_cast<CriticalFn>(backup), std::memory_order_release);
    else if (std::strcmp(resource_id, kResourceDefense) == 0)
        g_defense_backup.store(reinterpret_cast<DefenseFn>(backup), std::memory_order_release);
    else if (std::strcmp(resource_id, kResourcePhysical) == 0)
        g_physical_backup.store(reinterpret_cast<DefenseFn>(backup), std::memory_order_release);
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
