#include "host/core_patches.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/plugin.h"
#include "patch/call_patch.h"
#include "infra/log/qol_log.h"
#include "game/access/patches.h"
#include "game/access/game_access.h"
#include "hooks/registry.h"
#include "patch/instr_patch.h"

// 本文件只保留宿主兼容 patch。S2 数量布局、直接位读重定向与 clamp 全部由
// plugins/stack_limit 的 999 原子组声明，避免 boot 阶段把数量功能变成 fatal 依赖。

namespace qol::host {
namespace {

constexpr const char* kOwner = "host";
constexpr const char* kReason = "Hive 登录跳过";
constexpr const char* kHubClaimId = "P_HUB_LOGIN_SKIP";

const contracts::ResourceClaim kHubClaim{
    kHubClaimId,
    kHubClaimId,
    contracts::ResourceMechanism::kCallPatch,
    contracts::ResourcePolicy::kPrivate,
    contracts::ResourceRequirement::kOptional,
    contracts::ResourceFailure::kFailSoft,
    true,
    kOwner,
    kReason,
    nullptr,
    120,
    nullptr,
    false,
};

constexpr const char* kMonsterClaimId = "patch:MONSTER_ITEM_COUNT_COMPAT";

std::atomic<bool> g_installed{false};
std::atomic<bool> g_installing{false};

bool install_hub() {
    qol::game::PatchSite site{};
    if (!qol::game::patch_site(kHubClaim.symbol, &site)) {
        QOL_LOG_WARN(QolDomain::kCore, "core_patches: hub site not found");
        return false;
    }
    const qol::patch::Outcome outcome = qol::patch::install_claim(kHubClaim, site.addr);
    if (outcome.ok != 0) {
        QOL_LOG_WARN(QolDomain::kCore, "core_patches: hub install failed: %s",
                     outcome.error != nullptr ? outcome.error : "?");
        return false;
    }
    hooks::set_state(kHubClaim.id, contracts::ResourceState::kLive);
    hooks::runtime_set(kHubClaim.id, site.addr, nullptr);
    return true;
}

// monster 版（大黄猫改版）物品数量上界兼容：mod 在 SAVE_LoadFile 内用自写包装器逐件
// 校验物品记录，对 I_COUNT 最高字节要求 ((b-2)>>1) <= 0x62；S2 扩展数量编码会改变该
// 字节使扩展数量物品被拒 → SAVE_LoadFile=0 → 角色未加载 → EnterPlay 崩溃。按特征字节
// 放宽 cmp w8,#0x62 → #0x7F。仅在 libgame.so 可执行映射内匹配；非 monster 版无操作。
void apply_monster_compat() {
    if (g_base == 0) return;
    static const uint8_t kSig[8] = {0x1F, 0x89, 0x01, 0x71, 0x68, 0x00, 0x00, 0x54};
    constexpr uint32_t kOrig = 0x7101891Fu;
    constexpr uint32_t kRepl = 0x7101FD1Fu;
    int hits = 0;
    FILE* f = fopen("/proc/self/maps", "r");
    if (f == nullptr) return;
    char line[512];
    while (fgets(line, sizeof(line), f) != nullptr) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {0};
        char path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &lo, &hi, perms, path) < 4) continue;
        if (strstr(path, "libgame.so") == nullptr || perms[2] != 'x') continue;
        for (uintptr_t p = lo; p + sizeof(kSig) <= hi; ++p) {
            if (memcmp(reinterpret_cast<const void*>(p), kSig, sizeof(kSig)) != 0) continue;
            const uint32_t word = *reinterpret_cast<const uint32_t*>(p);
            if (word == kRepl) {
                ++hits;
                continue;
            }
            if (word != kOrig) continue;
            const qol::patch::Outcome outcome =
                qol::patch::install_word_at(kMonsterClaimId, p, kOrig, kRepl);
            if (outcome.ok == 0) {
                ++hits;
                QOL_LOG_INFO(QolDomain::kCore,
                             "core_patches monster compat: 0x%lx cmp w8,#0x62 -> #0x7F",
                             static_cast<unsigned long>(p));
            } else {
                QOL_LOG_WARN(QolDomain::kCore, "core_patches monster compat failed 0x%lx: %s",
                             static_cast<unsigned long>(p),
                             outcome.error != nullptr ? outcome.error : "?");
            }
        }
    }
    fclose(f);
    if (hits == 0) {
        QOL_LOG_INFO(QolDomain::kCore, "core_patches monster compat: no target (non-monster build)");
    }
}

}  // namespace

bool core_patches_install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (g_installing.exchange(true, std::memory_order_acq_rel)) {
        return g_installed.load(std::memory_order_acquire);
    }

    patch::set_primitives(qol::patch::Primitives{
        &call_patch_install_bl,
        &call_patch_revert_bl,
        &call_patch_install_word,
        &call_patch_revert_word,
    });

    // boot hook 只负责宿主兼容；g_base 尚未就绪时允许 bridge_init 后续重试。
    if (g_base == 0) {
        g_installing.store(false, std::memory_order_release);
        return true;
    }

    hooks::register_claim(kHubClaim);
    (void)install_hub();
    apply_monster_compat();
    g_installed.store(true, std::memory_order_release);
    g_installing.store(false, std::memory_order_release);
    return true;
}

}  // namespace qol::host
