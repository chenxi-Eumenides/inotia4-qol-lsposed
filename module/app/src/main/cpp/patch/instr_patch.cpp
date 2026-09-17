#include "patch/instr_patch.h"

#include "game/access/patches.h"
#include "patch/claims.h"

namespace qol::patch {
namespace {

Primitives g_primitives{};
bool g_primitives_set = false;

Outcome failure(const char* error) {
    return Outcome{1, error};
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
    // BL 重定向必须有 wrapper；字替换没有可透传执行体，handler 必须为空。
    const bool bl = site.kind == qol::game::PatchKind::kBlRedirect;
    if ((bl && claim.handler == nullptr) || (!bl && claim.handler != nullptr)) {
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

Outcome install_claim(const contracts::ResourceClaim& claim, uintptr_t site_addr) {
    qol::game::PatchSite site{};
    Outcome error{};
    if (!resolve_site(claim, site_addr, &site, &error)) {
        return error;
    }
    if (!claim_point(site.addr, claim.id)) {
        return failure("point already claimed");
    }

    bool applied = false;
    if (site.kind == qol::game::PatchKind::kBlRedirect) {
        applied = g_primitives.install_bl != nullptr &&
                  g_primitives.install_bl(site.addr, site.expected_word, claim.handler);
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

Outcome release_claim(const contracts::ResourceClaim& claim, uintptr_t site_addr) {
    qol::game::PatchSite site{};
    Outcome error{};
    if (!resolve_site(claim, site_addr, &site, &error)) {
        return error;
    }
    // §4.2 物理归还：先还原原字，成功后才释放占用。还原失败时保留占用——否则点可被
    // 重新声明，而旧指令字可能仍在（tombstone 语义丢失），由引擎按 tombstone + 隔离处理。
    bool applied = false;
    if (site.kind == qol::game::PatchKind::kBlRedirect) {
        applied = g_primitives.revert_bl != nullptr &&
                  g_primitives.revert_bl(site.addr, site.expected_word);
    } else {
        applied = g_primitives.revert_word != nullptr &&
                  g_primitives.revert_word(site.addr, site.expected_word);
    }
    if (!applied) {
        return failure("primitive rejected");
    }
    release_point(site.addr);
    return Outcome{0, nullptr};
}

}  // namespace qol::patch
