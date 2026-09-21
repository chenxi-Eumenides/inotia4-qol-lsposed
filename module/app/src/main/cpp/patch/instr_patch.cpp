#include "patch/instr_patch.h"

#include "game/access/patches.h"
#include "patch/claims.h"

namespace qol::patch {
namespace {

Primitives g_primitives{};
bool g_primitives_set = false;
// BL imm26 可达范围（±128MB）；本层在安装前收口，避免落到近址 thunk 路径。
constexpr int64_t kBlRange = 0x08000000LL;

Outcome failure(const char* error) {
    return Outcome{1, error};
}

// 同 .so 内 BL 直编可达性：4 字节对齐 + imm26 范围。不足即 fail-closed（不分配 thunk）。
bool bl_target_encodable(uintptr_t call_addr, uintptr_t target) {
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(call_addr);
    return (delta & 0x3) == 0 && delta > -kBlRange && delta < kBlRange;
}

// 站点解析 + 地址一致性 + kind/handler 匹配；任一不满足写出静态错误文本。
bool resolve_site(const contracts::ResourceClaim& claim, uintptr_t site_addr,
                  qol::game::PatchSite* out, Outcome* error) {
    if (!g_primitives_set) {
        *error = failure("primitives not set");
        return false;
    }
    if (claim.symbol == nullptr) {
        *error = failure("site not found");
        return false;
    }
    qol::game::PatchSite site{};
    if (!qol::game::patch_site(claim.symbol, &site)) {
        *error = failure("site not found");
        return false;
    }
    if (site.addr != site_addr) {
        *error = failure("site address mismatch");
        return false;
    }
    // 只有 handler 型重定向（BL / B 尾调用）需要 wrapper；字替换与符号型 BL 没有可透传
    // 执行体，handler 必须为空。
    const bool needs_handler = site.kind == qol::game::PatchKind::kBlRedirect ||
                               site.kind == qol::game::PatchKind::kBranchRedirect ||
                               site.kind == qol::game::PatchKind::kBranchRedirectWithReturn;
    const bool handler_ok =
        needs_handler ? (claim.handler != nullptr) : (claim.handler == nullptr);
    if (!handler_ok) {
        *error = failure("kind mismatch");
        return false;
    }
    *out = site;
    return true;
}

}  // namespace

void set_primitives(const Primitives& primitives) {
    g_primitives = primitives;
    g_primitives_set = true;
}

const Primitives* primitives() {
    return g_primitives_set ? &g_primitives : nullptr;
}

Outcome install_claim(const contracts::ResourceClaim& claim, uintptr_t site_addr) {
    qol::game::PatchSite site{};
    Outcome error{};
    if (!resolve_site(claim, site_addr, &site, &error)) {
        return error;
    }
    // 符号型 BL 目标可达性预检：越界/未对齐 fail-closed，不落到 thunk 分配。
    if (site.kind == qol::game::PatchKind::kBlRedirectToSymbol &&
        !bl_target_encodable(site.addr, site.redirect_target)) {
        return failure("branch target out of BL range");
    }
    if (!claim_point(site.addr, claim.id)) {
        return failure("point already claimed");
    }

    if (site.kind == qol::game::PatchKind::kBranchRedirect) {
        // B 尾调用：宿主在近址池分配 thunk 并把站点 B 改指 thunk，返回 thunk 运行期地址；
        // 本层收口 B 编码可达性（thunk 必须落在站点 ±128MB 内）。
        const uintptr_t thunk =
            g_primitives.install_branch != nullptr
                ? g_primitives.install_branch(site.addr, site.expected_word, claim.handler)
                : 0;
        if (thunk == 0) {
            release_point(site.addr);
            return failure("primitive rejected");
        }
        if (!bl_target_encodable(site.addr, thunk)) {
            // 宿主返回越界 thunk：撤回已写 B，释放占用，不半装。
            if (g_primitives.revert_branch != nullptr) {
                g_primitives.revert_branch(site.addr, site.expected_word);
            }
            release_point(site.addr);
            return failure("branch target out of BL range");
        }
        return Outcome{0, nullptr};
    }

    if (site.kind == qol::game::PatchKind::kBranchRedirectWithReturn) {
        const bool applied = g_primitives.install_branch_with_return != nullptr &&
            g_primitives.install_branch_with_return(
                site.addr, site.expected_word, site.alternate_expected_word, claim.handler,
                site.return_addr, site.tail_word);
        if (!applied) {
            release_point(site.addr);
            return failure("primitive rejected");
        }
        return Outcome{0, nullptr};
    }

    bool applied = false;
    if (site.kind == qol::game::PatchKind::kBlRedirect) {
        applied = g_primitives.install_bl != nullptr &&
                  g_primitives.install_bl(site.addr, site.expected_word, claim.handler);
    } else if (site.kind == qol::game::PatchKind::kBlRedirectToSymbol) {
        applied = g_primitives.install_bl != nullptr &&
                  g_primitives.install_bl(site.addr, site.expected_word,
                                          reinterpret_cast<void*>(site.redirect_target));
    } else {
        applied = g_primitives.install_word != nullptr &&
                  g_primitives.install_word(site.addr, site.expected_word, site.replacement_word);
    }
    if (!applied) {
        // 未生效即撤销占用：否则失败安装会把地址锁死给新所有者。
        release_point(site.addr);
        return failure("primitive rejected");
    }
    return Outcome{0, nullptr};
}

Outcome install_word_at(const char* claim_id, uintptr_t addr, uint32_t expected_word,
                        uint32_t new_word) {
    if (!g_primitives_set) {
        return failure("primitives not set");
    }
    if (claim_id == nullptr || addr == 0) {
        return failure("site not found");
    }
    if (!claim_point(addr, claim_id)) {
        return failure("point already claimed");
    }
    if (g_primitives.install_word == nullptr ||
        !g_primitives.install_word(addr, expected_word, new_word)) {
        release_point(addr);
        return failure("primitive rejected");
    }
    return Outcome{0, nullptr};
}

Outcome release_claim(const contracts::ResourceClaim& claim, uintptr_t site_addr) {
    qol::game::PatchSite site{};
    Outcome error{};
    if (!resolve_site(claim, site_addr, &site, &error)) {
        return error;
    }
    // §4.2 物理归还：先还原原字，成功后才释放占用。还原失败时保留占用——否则点可被
    // 重新声明，而旧指令字可能仍在（tombstone 语义丢失），由引擎按 tombstone + 隔离处理。
    bool applied = false;
    if (site.kind == qol::game::PatchKind::kWordReplace) {
        applied = g_primitives.revert_word != nullptr &&
                  g_primitives.revert_word(site.addr, site.expected_word);
    } else if (site.kind == qol::game::PatchKind::kBranchRedirect) {
        applied = g_primitives.revert_branch != nullptr &&
                  g_primitives.revert_branch(site.addr, site.expected_word);
    } else if (site.kind == qol::game::PatchKind::kBranchRedirectWithReturn) {
        applied = g_primitives.revert_branch_with_return != nullptr &&
                  g_primitives.revert_branch_with_return(
                      site.addr, site.expected_word, site.alternate_expected_word);
    } else {
        applied = g_primitives.revert_bl != nullptr &&
                  g_primitives.revert_bl(site.addr, site.expected_word);
    }
    if (!applied) {
        return failure("primitive rejected");
    }
    release_point(site.addr);
    return Outcome{0, nullptr};
}

}  // namespace qol::patch
