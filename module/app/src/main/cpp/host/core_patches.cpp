#include "host/core_patches.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/plugin.h"
#include "core/native/call_patch.h"
#include "core/native/qol_log.h"
#include "game/access/patches.h"
#include "game_access.h"
#include "hooks/registry.h"
#include "patch/instr_patch.h"

// 启动期常驻 patch 的声明式安装实现（见 .h）。所有站点地址/指令字来自
// game/access/patches 的登记表（常量单源 game_symbols.h），本文件不写裸地址。
//
// monster 兼容是扫描式例外：地址由运行时特征字节匹配得出，无法静态登记，
// 故直接经 patch/instr_patch 的 install_word_at 占用同一张点表，不进 hooks/registry
// 资源表；仍属同一占用仲裁与原语，失败仅记录（fail-soft）。

namespace qol::host {
namespace {

constexpr const char* kOwner = "core";
constexpr const char* kReason =
    "常驻指令 patch：S2 数量布局/直接位读重定向/上限 clamp/登录弹窗跳过；"
    "调用点无函数指针槽可挂，需改指令字或 BL 目标";
constexpr const char* kMonsterClaimId = "patch:MONSTER_ITEM_COUNT_COMPAT";

// 12 条必需（fail-closed）+ 1 条 hub（fail-soft）。顺序 = 安装顺序：
// 位段缩窄 ×2 → S2 重定向 arg/call ×10 → hub ×1。
#define CORE_PATCH_CLAIM(NAME, REQ, FAIL)                                             \
    {NAME, NAME, contracts::ResourceMechanism::kCallPatch,                            \
     contracts::ResourcePolicy::kPrivate, REQ, FAIL, true, kOwner, kReason, nullptr,  \
     120, nullptr}

const contracts::ResourceClaim kClaims[] = {
    CORE_PATCH_CLAIM("P_SAVE_SAVE_INVENTORY_BITEND", contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_SAVE_LOAD_INVENTORY_BITEND", contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18D0_ARG",
                     contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18D0_CALL",
                     contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18E8_ARG",
                     contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18E8_CALL",
                     contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_UISTORE_SELL_ITEM_D26B8_ARG", contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_UISTORE_SELL_ITEM_D26B8_CALL", contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_ITEMSYSTEM_DIVIDE_108458_ARG", contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_ITEMSYSTEM_DIVIDE_108458_CALL", contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_ITEMSYSTEM_DIVIDE_10848C_ARG", contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_ITEMSYSTEM_DIVIDE_10848C_CALL", contracts::ResourceRequirement::kRequired,
                     contracts::ResourceFailure::kFailClosed),
    CORE_PATCH_CLAIM("P_HUB_LOGIN_SKIP", contracts::ResourceRequirement::kOptional,
                     contracts::ResourceFailure::kFailSoft),
};
#undef CORE_PATCH_CLAIM

constexpr uint32_t kRequiredCount = 12;
constexpr uint32_t kHubIndex = 12;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_installing{false};

// 解析站点并物理安装；成功后置 live 并登记运行期地址。
bool install_site(const contracts::ResourceClaim& claim) {
    qol::game::PatchSite site{};
    if (!qol::game::patch_site(claim.symbol, &site)) {
        QOL_LOG_ERROR(QolDomain::kCore, "core_patches: site not found %s", claim.symbol);
        return false;
    }
    const qol::patch::Outcome outcome = qol::patch::install_claim(claim, site.addr);
    if (outcome.ok != 0) {
        QOL_LOG_ERROR(QolDomain::kCore, "core_patches: install failed %s: %s", claim.id,
                      outcome.error != nullptr ? outcome.error : "?");
        return false;
    }
    hooks::set_state(claim.id, contracts::ResourceState::kLive);
    hooks::runtime_set(claim.id, site.addr, nullptr);
    return true;
}

// 物理释放；成功置 free、失败置 tombstone + 隔离（与宿主回滚语义一致）。
void release_site(const contracts::ResourceClaim& claim) {
    qol::game::PatchSite site{};
    const bool released =
        qol::game::patch_site(claim.symbol, &site) &&
        qol::patch::release_claim(claim, site.addr).ok == 0;
    if (released) {
        hooks::set_state(claim.id, contracts::ResourceState::kFree);
        hooks::runtime_clear(claim.id);
    } else {
        hooks::set_state(claim.id, contracts::ResourceState::kTombstone);
        hooks::isolated_set(claim.id, true);
    }
}

// monster 版（大黄猫改版）物品数量上界兼容：mod 在 SAVE_LoadFile 内用自写包装器逐件
// 校验物品记录，对 I_COUNT 最高字节要求 ((b-2)>>1) <= 0x62；S2 扩展数量编码会改变该
// 字节使扩展数量物品被拒 → SAVE_LoadFile=0 → 角色未加载 → EnterPlay 崩溃。按特征字节
// 放宽 cmp w8,#0x62 → #0x7F。仅在 libgame.so 可执行映射内匹配；非 monster 版无操作。
void apply_monster_compat() {
    if (g_base == 0) return;
    static const uint8_t kSig[8] = {0x1F, 0x89, 0x01, 0x71, 0x68, 0x00, 0x00, 0x54};
    constexpr uint32_t kOrig = 0x7101891Fu;  // cmp w8, #0x62
    constexpr uint32_t kRepl = 0x7101FD1Fu;  // cmp w8, #0x7F
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
                QOL_LOG_ERROR(QolDomain::kCore, "core_patches monster compat failed 0x%lx: %s",
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

    // patch 原语注入：本入口在 assembly 装配前、bridge_init 内被调用，须自包含
    // （与 host/frame_source 同构；P5 原语迁入 patch/ 后两处一并改指）。
    patch::set_primitives(qol::patch::Primitives{
        &call_patch_install_bl,
        &call_patch_revert_bl,
        &call_patch_install_word,
        &call_patch_revert_word,
    });

    if (!bridge_ready() || g_base == 0) {
        g_installing.store(false, std::memory_order_release);
        return false;  // 未就绪：不改写字节，允许后续重试
    }

    for (const contracts::ResourceClaim& claim : kClaims) {
        if (!hooks::register_claim(claim)) {
            QOL_LOG_ERROR(QolDomain::kCore, "core_patches: claim registration failed %s", claim.id);
            g_installing.store(false, std::memory_order_release);
            return false;
        }
    }

    // 必需 12 条：全成或全退（fail-closed，禁止半装）。
    uint32_t installed = 0;
    for (; installed < kRequiredCount; ++installed) {
        if (!install_site(kClaims[installed])) break;
    }
    if (installed < kRequiredCount) {
        for (uint32_t i = installed; i > 0; --i) release_site(kClaims[i - 1]);
        hooks::set_state(kClaims[installed].id, contracts::ResourceState::kTombstone);
        hooks::isolated_set(kClaims[installed].id, true);
        QOL_LOG_ERROR(QolDomain::kCore, "core_patches: fail-closed at %s",
                      kClaims[installed].id);
        g_installing.store(false, std::memory_order_release);
        return false;
    }

    apply_monster_compat();

    // hub 软失败：登录弹窗跳过是体验优化，匹配失败只记日志、不阻断 bridge。
    const contracts::ResourceClaim& hub = kClaims[kHubIndex];
    if (install_site(hub)) {
        QOL_LOG_INFO(QolDomain::kCore, "core_patches installed %u + hub", kRequiredCount);
    } else {
        QOL_LOG_WARN(QolDomain::kCore, "core_patches hub skipped (soft fail) %s", hub.id);
    }

    g_installed.store(true, std::memory_order_release);
    g_installing.store(false, std::memory_order_release);
    return true;
}

}  // namespace qol::host
