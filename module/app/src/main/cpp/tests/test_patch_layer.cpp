// patch/claims.h + patch/instr_patch.h + game/access/patches.h host 单测：
// 覆盖占用声明仲裁（重复/幂等/容量/释放）与指令 patch 安装/还原的
// 站点校验、kind/handler 匹配、原语拒绝与占用释放。

#include "patch/claims.h"
#include "patch/instr_patch.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/plugin.h"
#include "data/native/game_symbols.h"
#include "game/access/patches.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// patches.cpp 依赖的游戏基址；测试可控（game/access/patches.cpp 取 g_base + VMA + 偏移）。
uintptr_t g_base = 0;

using qol::contracts::ResourceClaim;
using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourcePolicy;
using qol::contracts::ResourceRequirement;
using qol::game::PatchSite;
using qol::patch::claim_point;
using qol::patch::claimed_count;
using qol::patch::claims_reset_for_test;
using qol::patch::install_claim;
using qol::patch::Outcome;
using qol::patch::point_owner;
using qol::patch::Primitives;
using qol::patch::release_claim;
using qol::patch::release_point;
using qol::patch::set_primitives;

namespace {

constexpr uintptr_t kBase = 0x10000000;

int g_dummy_handler = 0;
int g_other_handler = 0;

struct FakePrims {
    int install_bl_calls = 0;
    int revert_bl_calls = 0;
    int install_word_calls = 0;
    int revert_word_calls = 0;
    bool install_bl_ok = true;
    bool revert_bl_ok = true;
    bool install_word_ok = true;
    bool revert_word_ok = true;
    uintptr_t last_addr = 0;
    uint32_t last_expected = 0;
    uint32_t last_new_word = 0;
    void* last_wrapper = nullptr;
};

FakePrims g_prims;

bool fake_install_bl(uintptr_t addr, uint32_t expected_word, void* wrapper) {
    ++g_prims.install_bl_calls;
    g_prims.last_addr = addr;
    g_prims.last_expected = expected_word;
    g_prims.last_wrapper = wrapper;
    return g_prims.install_bl_ok;
}

bool fake_revert_bl(uintptr_t addr, uint32_t original_word) {
    ++g_prims.revert_bl_calls;
    g_prims.last_addr = addr;
    g_prims.last_expected = original_word;
    return g_prims.revert_bl_ok;
}

bool fake_install_word(uintptr_t addr, uint32_t expected_word, uint32_t new_word) {
    ++g_prims.install_word_calls;
    g_prims.last_addr = addr;
    g_prims.last_expected = expected_word;
    g_prims.last_new_word = new_word;
    return g_prims.install_word_ok;
}

bool fake_revert_word(uintptr_t addr, uint32_t original_word) {
    ++g_prims.revert_word_calls;
    g_prims.last_addr = addr;
    g_prims.last_expected = original_word;
    return g_prims.revert_word_ok;
}

void use_fake_primitives() {
    set_primitives(Primitives{&fake_install_bl, &fake_revert_bl, &fake_install_word,
                              &fake_revert_word});
}

void reset_all() {
    claims_reset_for_test();
    g_prims = FakePrims{};
    g_base = kBase;
    use_fake_primitives();
}

uintptr_t party_addr() {
    return g_base + F_CHAR_MOVE_VMA + CHAR_MOVE_PUSH_PARTY_BL_OFF;
}

uintptr_t stand_addr() {
    return g_base + F_CHAR_MOVE_VMA + CHAR_MOVE_PUSH_STAND_CBZ_OFF;
}

ResourceClaim make_patch_claim(const char* id, const char* symbol, void* handler) {
    ResourceClaim claim{};
    claim.id = id;
    claim.symbol = symbol;
    claim.mechanism = ResourceMechanism::kCallPatch;
    claim.policy = ResourcePolicy::kPrivate;
    claim.requirement = ResourceRequirement::kRequired;
    claim.failure = ResourceFailure::kFailClosed;
    claim.resumable = true;
    claim.owner = "test";
    claim.reason = "test";
    claim.gate = nullptr;
    claim.step_budget = 120;
    claim.handler = handler;
    return claim;
}

bool error_is(const Outcome& outcome, const char* expected) {
    return outcome.ok != 0 && outcome.error != nullptr &&
           std::strcmp(outcome.error, expected) == 0;
}

void test_claims_arbitration() {
    reset_all();
    CHECK(point_owner(0x2000) == nullptr);
    CHECK(claimed_count() == 0);

    CHECK(claim_point(0x2000, "claim:a"));
    CHECK(claimed_count() == 1);
    CHECK(point_owner(0x2000) != nullptr);
    CHECK(std::strcmp(point_owner(0x2000), "claim:a") == 0);

    CHECK(claim_point(0x2000, "claim:a"));   // 同 claim 幂等
    CHECK(claimed_count() == 1);
    CHECK(!claim_point(0x2000, "claim:b"));  // 不同 claim 冲突
    CHECK(claimed_count() == 1);

    CHECK(!claim_point(0x2000, nullptr));
    CHECK(!claim_point(0x2000, ""));

    CHECK(release_point(0x2000));
    CHECK(!release_point(0x2000));  // 未占用
    CHECK(point_owner(0x2000) == nullptr);
    CHECK(claimed_count() == 0);

    // 释放后可被新所有者占用。
    CHECK(claim_point(0x2000, "claim:b"));
    CHECK(claimed_count() == 1);
}

void test_claims_capacity() {
    reset_all();
    for (uint32_t i = 0; i < 64; ++i) {
        CHECK(claim_point(0x3000 + i * 4, "owner:idempotent"));
    }
    CHECK(claimed_count() == 64);
    CHECK(!claim_point(0x4000, "overflow"));
    CHECK(claimed_count() == 64);
    // 满容时同地址同 claim 幂等仍成立。
    CHECK(claim_point(0x3000, "owner:idempotent"));
    CHECK(claimed_count() == 64);
}

void test_primitives_not_set() {
    // 必须在任何 set_primitives 之前运行。
    claims_reset_for_test();
    const ResourceClaim claim =
        make_patch_claim("patch:x", "P_CHAR_MOVE_PUSH_PARTY", &g_dummy_handler);
    g_base = kBase;
    const Outcome outcome = install_claim(claim, party_addr());
    CHECK(error_is(outcome, "primitives not set"));
}

void test_install_bl_success() {
    reset_all();
    const ResourceClaim claim =
        make_patch_claim("patch:a", "P_CHAR_MOVE_PUSH_PARTY", &g_dummy_handler);
    const uintptr_t addr = party_addr();

    PatchSite site{};
    CHECK(qol::game::patch_site("P_CHAR_MOVE_PUSH_PARTY", &site));
    CHECK(site.addr == addr);
    CHECK(site.kind == qol::game::PatchKind::kBlRedirect);
    CHECK(site.expected_word == CHAR_MOVE_PUSH_PARTY_BL_WORD);

    const Outcome outcome = install_claim(claim, addr);
    CHECK(outcome.ok == 0);
    CHECK(outcome.error == nullptr);
    CHECK(g_prims.install_bl_calls == 1);
    CHECK(g_prims.install_word_calls == 0);
    CHECK(g_prims.last_addr == addr);
    CHECK(g_prims.last_expected == CHAR_MOVE_PUSH_PARTY_BL_WORD);
    CHECK(g_prims.last_wrapper == &g_dummy_handler);
    CHECK(point_owner(addr) != nullptr);
    CHECK(std::strcmp(point_owner(addr), "patch:a") == 0);
}

void test_install_word_success() {
    reset_all();
    const ResourceClaim claim = make_patch_claim("patch:b", "P_CHAR_MOVE_PUSH_STAND", nullptr);
    const uintptr_t addr = stand_addr();

    PatchSite site{};
    CHECK(qol::game::patch_site("P_CHAR_MOVE_PUSH_STAND", &site));
    CHECK(site.kind == qol::game::PatchKind::kWordReplace);
    CHECK(site.replacement_word == ARM64_NOP_WORD);

    const Outcome outcome = install_claim(claim, addr);
    CHECK(outcome.ok == 0);
    CHECK(g_prims.install_word_calls == 1);
    CHECK(g_prims.install_bl_calls == 0);
    CHECK(g_prims.last_expected == CHAR_MOVE_PUSH_STAND_CBZ_WORD);
    CHECK(g_prims.last_new_word == ARM64_NOP_WORD);
    CHECK(std::strcmp(point_owner(addr), "patch:b") == 0);
}

void test_kind_mismatch() {
    reset_all();
    // BL 点缺 handler。
    const ResourceClaim bl = make_patch_claim("patch:a", "P_CHAR_MOVE_PUSH_PARTY", nullptr);
    CHECK(error_is(install_claim(bl, party_addr()), "kind mismatch"));
    CHECK(g_prims.install_bl_calls == 0);
    // 字替换点带 handler。
    const ResourceClaim word =
        make_patch_claim("patch:b", "P_CHAR_MOVE_PUSH_STAND", &g_dummy_handler);
    CHECK(error_is(install_claim(word, stand_addr()), "kind mismatch"));
    CHECK(g_prims.install_word_calls == 0);
    CHECK(claimed_count() == 0);
}

void test_site_and_address_errors() {
    reset_all();
    const ResourceClaim unknown =
        make_patch_claim("patch:x", "P_NOT_REGISTERED", &g_dummy_handler);
    CHECK(error_is(install_claim(unknown, party_addr()), "site not found"));

    const ResourceClaim claim =
        make_patch_claim("patch:a", "P_CHAR_MOVE_PUSH_PARTY", &g_dummy_handler);
    CHECK(error_is(install_claim(claim, party_addr() + 4), "site address mismatch"));

    // g_base==0：站点不可解析。
    g_base = 0;
    CHECK(error_is(install_claim(claim, party_addr()), "site not found"));
    g_base = kBase;
    CHECK(claimed_count() == 0);
    CHECK(g_prims.install_bl_calls == 0);
}

void test_point_already_claimed() {
    reset_all();
    const ResourceClaim first =
        make_patch_claim("patch:a", "P_CHAR_MOVE_PUSH_PARTY", &g_dummy_handler);
    CHECK(install_claim(first, party_addr()).ok == 0);

    const ResourceClaim second =
        make_patch_claim("patch:b", "P_CHAR_MOVE_PUSH_PARTY", &g_other_handler);
    CHECK(error_is(install_claim(second, party_addr()), "point already claimed"));
    CHECK(g_prims.install_bl_calls == 1);
    CHECK(std::strcmp(point_owner(party_addr()), "patch:a") == 0);
}

void test_primitive_rejected_releases_claim() {
    reset_all();
    const ResourceClaim bl =
        make_patch_claim("patch:a", "P_CHAR_MOVE_PUSH_PARTY", &g_dummy_handler);
    g_prims.install_bl_ok = false;
    CHECK(error_is(install_claim(bl, party_addr()), "primitive rejected"));
    CHECK(point_owner(party_addr()) == nullptr);  // 安装未生效不留占用
    CHECK(claimed_count() == 0);

    const ResourceClaim word = make_patch_claim("patch:b", "P_CHAR_MOVE_PUSH_STAND", nullptr);
    g_prims.install_word_ok = false;
    CHECK(error_is(install_claim(word, stand_addr()), "primitive rejected"));
    CHECK(point_owner(stand_addr()) == nullptr);
    CHECK(claimed_count() == 0);
}

void test_release_success() {
    reset_all();
    const ResourceClaim bl =
        make_patch_claim("patch:a", "P_CHAR_MOVE_PUSH_PARTY", &g_dummy_handler);
    CHECK(install_claim(bl, party_addr()).ok == 0);
    const Outcome bl_release = release_claim(bl, party_addr());
    CHECK(bl_release.ok == 0);
    CHECK(bl_release.error == nullptr);
    CHECK(g_prims.revert_bl_calls == 1);
    CHECK(g_prims.last_addr == party_addr());
    CHECK(g_prims.last_expected == CHAR_MOVE_PUSH_PARTY_BL_WORD);
    CHECK(point_owner(party_addr()) == nullptr);

    const ResourceClaim word = make_patch_claim("patch:b", "P_CHAR_MOVE_PUSH_STAND", nullptr);
    CHECK(install_claim(word, stand_addr()).ok == 0);
    CHECK(release_claim(word, stand_addr()).ok == 0);
    CHECK(g_prims.revert_word_calls == 1);
    CHECK(g_prims.last_expected == CHAR_MOVE_PUSH_STAND_CBZ_WORD);
    CHECK(point_owner(stand_addr()) == nullptr);
}

void test_release_primitive_rejected_keeps_owner() {
    reset_all();
    const ResourceClaim word = make_patch_claim("patch:b", "P_CHAR_MOVE_PUSH_STAND", nullptr);
    CHECK(install_claim(word, stand_addr()).ok == 0);

    // 物理还原失败：保留占用（§4.2），点不得被重新声明。
    g_prims.revert_word_ok = false;
    const Outcome outcome = release_claim(word, stand_addr());
    CHECK(error_is(outcome, "primitive rejected"));
    CHECK(point_owner(stand_addr()) != nullptr);
    CHECK(std::strcmp(point_owner(stand_addr()), "patch:b") == 0);
    CHECK(g_prims.revert_word_calls == 1);

    const ResourceClaim other = make_patch_claim("patch:c", "P_CHAR_MOVE_PUSH_STAND", nullptr);
    CHECK(error_is(install_claim(other, stand_addr()), "point already claimed"));
    CHECK(std::strcmp(point_owner(stand_addr()), "patch:b") == 0);

    // 原语恢复后重试还原：成功后释放占用，点可被重新声明。
    g_prims.revert_word_ok = true;
    CHECK(release_claim(word, stand_addr()).ok == 0);
    CHECK(point_owner(stand_addr()) == nullptr);
    CHECK(claimed_count() == 0);
    CHECK(install_claim(other, stand_addr()).ok == 0);
    CHECK(std::strcmp(point_owner(stand_addr()), "patch:c") == 0);
}

}  // namespace

int main() {
    test_primitives_not_set();  // 必须在任何 set_primitives 之前
    test_claims_arbitration();
    test_claims_capacity();
    test_install_bl_success();
    test_install_word_success();
    test_kind_mismatch();
    test_site_and_address_errors();
    test_point_already_claimed();
    test_primitive_rejected_releases_claim();
    test_release_success();
    test_release_primitive_rejected_keeps_owner();
    std::printf("patch_layer: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
