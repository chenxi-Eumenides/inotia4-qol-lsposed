#pragma once
#include <cstdint>

namespace qol::capacity {

// 插件身份与单插件声明预算。
inline constexpr uint32_t kMaxPlugins = 128;
// 当前最大内置插件 31 条（extension_bag），保留约 2× 余量。
inline constexpr uint32_t kMaxPluginResources = 64;

// 全局资源声明预算：插件数 × 每插件平均资源预算。
// 平均预算 8 是当前内置插件实测值（约 5 条/插件）加余量；
// 实际内置总量由 host/assembly.cpp 的 constexpr 汇总做 static_assert。
inline constexpr uint32_t kResourceBudgetPerPlugin = 8;
inline constexpr uint32_t kMaxDeclaredResources = kMaxPlugins * kResourceBudgetPerPlugin;

// 宿主 claim 预留：save 3 + frame 2 + core_patches 3 = 8，另留一倍余量。
inline constexpr uint32_t kHostClaimCount = 8;
inline constexpr uint32_t kHostClaimReserve = 16;

// 机制表：每条声明资源最多占一个安装点。
inline constexpr uint32_t kMaxPatchPoints = kMaxDeclaredResources;
inline constexpr uint32_t kMaxCallPatches = kMaxDeclaredResources;
inline constexpr uint32_t kMaxShortHooks = kMaxDeclaredResources;
inline constexpr uint32_t kMaxInsnPatches = kMaxDeclaredResources;

// 共享 thunk 池：单条资源最多占 4 个 near slot（insn patch 多槽 thunk），
// 短跳 hook 占 2 个，call patch 占 1 个；按最坏情况派生总槽位。
inline constexpr uint32_t kMaxThunkSlotsPerClaim = 4;
inline constexpr uint32_t kMaxThunkSlots = kMaxThunkSlotsPerClaim * kMaxDeclaredResources;

// 事务：单事务只覆盖一个插件的资源；活跃事务数按插件数计算。
inline constexpr uint32_t kMaxActiveTransactions = kMaxPlugins;
inline constexpr uint32_t kMaxTerminalTransactions = kMaxActiveTransactions / 8;

// Feature 状态表：插件数 × 每插件 feature 上限 + 宿主 owner 预留。
inline constexpr uint32_t kMaxFeaturesPerPlugin = 8;
inline constexpr uint32_t kHostFeatureOwners = 4;
inline constexpr uint32_t kMaxFeatureStatuses =
    (kMaxPlugins + kHostFeatureOwners) * kMaxFeaturesPerPlugin;

static_assert(kMaxThunkSlots >= kMaxCallPatches,
              "thunk pool must cover one slot per call patch");
static_assert(kMaxThunkSlots >= 2 * kMaxShortHooks,
              "thunk pool must cover two near slots per short hook");
static_assert(kMaxThunkSlots >= kMaxThunkSlotsPerClaim * kMaxInsnPatches,
              "thunk pool must cover multi-slot insn patches");
static_assert(kMaxDeclaredResources >= kHostClaimReserve,
              "host claim reserve must fit in declared resource budget");

}  // namespace qol::capacity
