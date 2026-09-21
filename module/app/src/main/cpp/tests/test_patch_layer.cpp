// patch/claims.h + patch/instr_patch.h + patch/ptr_slot.h + game/access/patches.h host 单测：
// 覆盖占用声明仲裁（重复/幂等/容量/释放）、指令 patch 安装/还原的站点校验/kind/handler 匹配/
// 原语拒绝与占用释放，以及指针槽机制（kPtrSlot）的槽值校验、占用、归还与 tombstone 语义。

#include "patch/claims.h"
#include "patch/instr_patch.h"
#include "patch/ptr_slot.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/plugin.h"
#include "game/symbols/game_symbols.h"
#include "game/access/patches.h"
#include "game/access/ptr_slots.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// patches.cpp 依赖的游戏基址；测试可控（game/access/patches.cpp 取 g_base + VMA + 偏移）。
uintptr_t g_base = 0;

// kBlRedirectToSymbol 站点由 patches.cpp 经 fn_resolve 解析目标函数；本测试提供确定性
// stub：默认恒等返回 vma（目标地址 = g_base + vma）。bias/misalign 用于覆盖目标越界
// 与未对齐时的 fail-closed 分支。
static uintptr_t g_fn_resolve_bias = 0;
static uintptr_t g_fn_resolve_misalign = 0;

uintptr_t fn_resolve(const char* /*macro_name*/, uintptr_t vma) {
    return vma + g_fn_resolve_bias + g_fn_resolve_misalign;
}

using qol::contracts::ResourceClaim;
using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourcePolicy;
using qol::contracts::ResourceRequirement;
using qol::game::PatchSite;
using qol::game::PtrSlotSite;
using qol::patch::claim_point;
using qol::patch::claimed_count;
using qol::patch::claims_reset_for_test;
using qol::patch::install_claim;
using qol::patch::install_word_at;
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
    int install_branch_calls = 0;
    int revert_branch_calls = 0;
    bool install_bl_ok = true;
    bool revert_bl_ok = true;
    bool install_word_ok = true;
    bool revert_word_ok = true;
    bool install_branch_ok = true;
    bool revert_branch_ok = true;
    // 假 thunk 相对站点偏移：默认 +0x100（可达）；可设为超 ±128MB 覆盖越界分支。
    int64_t install_branch_thunk_delta = 0x100;
    uintptr_t last_addr = 0;
    uint32_t last_expected = 0;
    uint32_t last_new_word = 0;
    void* last_wrapper = nullptr;
    uintptr_t last_branch_thunk = 0;
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

// B 尾调用原语：返回站点附近（默认 +0x100）的 thunk 地址；0 = 分配失败。
uintptr_t fake_install_branch(uintptr_t addr, uint32_t expected_word, void* handler) {
    ++g_prims.install_branch_calls;
    g_prims.last_addr = addr;
    g_prims.last_expected = expected_word;
    g_prims.last_wrapper = handler;
    if (!g_prims.install_branch_ok) return 0;
    const uintptr_t thunk = static_cast<uintptr_t>(
        static_cast<int64_t>(addr) + g_prims.install_branch_thunk_delta);
    g_prims.last_branch_thunk = thunk;
    return thunk;
}

bool fake_revert_branch(uintptr_t addr, uint32_t original_word) {
    ++g_prims.revert_branch_calls;
    g_prims.last_addr = addr;
    g_prims.last_expected = original_word;
    return g_prims.revert_branch_ok;
}

// 指针槽原语：真实写入测试可控的槽存储，便于断言「写生效/未生效」与 tombstone 保留占用。
struct FakePtrPrims {
    int write_calls = 0;
    int restore_calls = 0;
    bool write_ok = true;
    bool restore_ok = true;
    uintptr_t last_addr = 0;
    void* last_value = nullptr;
};

FakePtrPrims g_ptr_prims;
void* g_ptr_slot_storage = nullptr;  // 模拟函数指针槽：site.slot_addr 指向它

bool fake_write_ptr(uintptr_t slot_addr, void* value) {
    ++g_ptr_prims.write_calls;
    g_ptr_prims.last_addr = slot_addr;
    g_ptr_prims.last_value = value;
    if (!g_ptr_prims.write_ok) return false;
    *reinterpret_cast<void**>(slot_addr) = value;
    return true;
}

bool fake_restore_ptr(uintptr_t slot_addr, void* value) {
    ++g_ptr_prims.restore_calls;
    g_ptr_prims.last_addr = slot_addr;
    g_ptr_prims.last_value = value;
    if (!g_ptr_prims.restore_ok) return false;
    *reinterpret_cast<void**>(slot_addr) = value;
    return true;
}

void use_fake_primitives() {
    set_primitives(Primitives{&fake_install_bl, &fake_revert_bl, &fake_install_word,
                              &fake_revert_word, &fake_write_ptr, &fake_restore_ptr,
                              &fake_install_branch, &fake_revert_branch});
}

void reset_all() {
    claims_reset_for_test();
    qol::game::patch_site_clear_overrides_for_test();
    g_prims = FakePrims{};
    g_ptr_prims = FakePtrPrims{};
    g_ptr_slot_storage = nullptr;
    g_base = kBase;
    g_fn_resolve_bias = 0;
    g_fn_resolve_misalign = 0;
    use_fake_primitives();
}

uintptr_t party_addr() {
    return g_base + F_CHAR_MOVE_VMA + CHAR_MOVE_PUSH_PARTY_BL_OFF;
}

uintptr_t stand_addr() {
    return g_base + F_CHAR_MOVE_VMA + CHAR_MOVE_PUSH_STAND_CBZ_OFF;
}

// B 尾调用站点：生产 kSites 尚无 B 站点（EB 站点留 D2a-3 登记），测试经覆盖表构造。
constexpr uintptr_t kBranchSiteAddr = kBase + 0x5000u;
constexpr uint32_t kBranchExpectedWord = 0x17ffff1eu;  // B 指令原字（测试构造）

bool arm_branch_site() {
    PatchSite site{};
    site.addr = kBranchSiteAddr;
    site.expected_word = kBranchExpectedWord;
    site.kind = qol::game::PatchKind::kBranchRedirect;
    return qol::game::patch_site_override_for_test("P_TEST_BRANCH", site);
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

ResourceClaim make_ptr_slot_claim(const char* id, void* handler) {
    ResourceClaim claim{};
    claim.id = id;
    claim.symbol = "ptrslot:TEST";
    claim.mechanism = ResourceMechanism::kPtrSlot;
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

PtrSlotSite ptr_site(void* expected) {
    PtrSlotSite site{};
    site.slot_addr = reinterpret_cast<uintptr_t>(&g_ptr_slot_storage);
    site.expected_value = expected;
    return site;
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

    void* storage = reinterpret_cast<void*>(&g_dummy_handler);
    g_ptr_slot_storage = storage;
    const ResourceClaim ptr_claim = make_ptr_slot_claim("ptrslot:x", &g_dummy_handler);
    CHECK(error_is(install_claim(ptr_claim, ptr_site(storage)), "primitives not set"));
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

void test_bl_redirect_to_symbol_success() {
    reset_all();
    PatchSite site{};
    CHECK(qol::game::patch_site("P_UISTORE_SELL_ITEM_D26B8_CALL", &site));
    CHECK(site.kind == qol::game::PatchKind::kBlRedirectToSymbol);
    CHECK(site.redirect_target == g_base + F_GET_CUMULATE_COUNT_VMA);
    const uintptr_t addr = g_base + F_UISTORE_SELL_ITEM_VMA + 0xd4;
    CHECK(site.addr == addr);
    CHECK(site.expected_word == 0x9401b799u);

    const ResourceClaim claim =
        make_patch_claim("patch:redir", "P_UISTORE_SELL_ITEM_D26B8_CALL", nullptr);
    const Outcome outcome = install_claim(claim, addr);
    CHECK(outcome.ok == 0);
    CHECK(g_prims.install_bl_calls == 1);
    CHECK(g_prims.install_word_calls == 0);
    CHECK(g_prims.last_addr == addr);
    CHECK(g_prims.last_expected == 0x9401b799u);
    CHECK(g_prims.last_wrapper == reinterpret_cast<void*>(g_base + F_GET_CUMULATE_COUNT_VMA));
    CHECK(std::strcmp(point_owner(addr), "patch:redir") == 0);

    CHECK(release_claim(claim, addr).ok == 0);
    CHECK(g_prims.revert_bl_calls == 1);
    CHECK(point_owner(addr) == nullptr);
}

void test_bl_redirect_to_symbol_mismatch() {
    reset_all();
    // 符号型 BL 无透传执行体：带 handler 必须拒绝。
    const ResourceClaim with_handler =
        make_patch_claim("patch:redir", "P_UISTORE_SELL_ITEM_D26B8_CALL", &g_dummy_handler);
    CHECK(error_is(install_claim(with_handler, g_base + F_UISTORE_SELL_ITEM_VMA + 0xd4),
                   "kind mismatch"));
    CHECK(g_prims.install_bl_calls == 0);
}

void test_bl_redirect_to_symbol_range_and_alignment() {
    const ResourceClaim claim =
        make_patch_claim("patch:redir", "P_UISTORE_SELL_ITEM_D26B8_CALL", nullptr);
    const uintptr_t addr = g_base + F_UISTORE_SELL_ITEM_VMA + 0xd4;

    // 越界：目标偏出 ±128MB（不落 thunk，直接 fail-closed）。
    reset_all();
    g_fn_resolve_bias = 0x10000000u;
    CHECK(error_is(install_claim(claim, addr), "branch target out of BL range"));
    CHECK(g_prims.install_bl_calls == 0);
    CHECK(point_owner(addr) == nullptr);

    // 未对齐：目标非 4 字节对齐。
    reset_all();
    g_fn_resolve_misalign = 2;
    CHECK(error_is(install_claim(claim, addr), "branch target out of BL range"));
    CHECK(g_prims.install_bl_calls == 0);
    CHECK(point_owner(addr) == nullptr);
}

void test_install_word_at() {
    reset_all();
    const uintptr_t addr = 0x12345678;
    CHECK(install_word_at("patch:scan", addr, 0x7101891Fu, 0x7101FD1Fu).ok == 0);
    CHECK(g_prims.install_word_calls == 1);
    CHECK(g_prims.last_addr == addr);
    CHECK(g_prims.last_expected == 0x7101891Fu);
    CHECK(g_prims.last_new_word == 0x7101FD1Fu);
    CHECK(std::strcmp(point_owner(addr), "patch:scan") == 0);

    // 同地址不同 owner → 占用冲突，不重复改写。
    CHECK(error_is(install_word_at("patch:other", addr, 0x7101891Fu, 0x7101FD1Fu),
                   "point already claimed"));
    CHECK(g_prims.install_word_calls == 1);

    // 原语拒绝：撤销占用，不留半装。
    reset_all();
    g_prims.install_word_ok = false;
    CHECK(error_is(install_word_at("patch:scan", addr, 0x7101891Fu, 0x7101FD1Fu),
                   "primitive rejected"));
    CHECK(point_owner(addr) == nullptr);

    // 空 claim_id / addr=0 拒绝。
    reset_all();
    CHECK(error_is(install_word_at(nullptr, addr, 0, 0), "site not found"));
    CHECK(error_is(install_word_at("patch:scan", 0, 0, 0), "site not found"));
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

// ---- kBranchRedirect B 尾调用机制 ----

// B 重定向成功：原语收到站点地址/原字/handler，占用登记在站点。
void test_install_branch_success() {
    reset_all();
    CHECK(arm_branch_site());
    const ResourceClaim claim = make_patch_claim("patch:br", "P_TEST_BRANCH", &g_dummy_handler);
    const Outcome outcome = install_claim(claim, kBranchSiteAddr);
    CHECK(outcome.ok == 0);
    CHECK(outcome.error == nullptr);
    CHECK(g_prims.install_branch_calls == 1);
    CHECK(g_prims.install_bl_calls == 0);
    CHECK(g_prims.install_word_calls == 0);
    CHECK(g_prims.last_addr == kBranchSiteAddr);
    CHECK(g_prims.last_expected == kBranchExpectedWord);
    CHECK(g_prims.last_wrapper == &g_dummy_handler);
    CHECK(point_owner(kBranchSiteAddr) != nullptr);
    CHECK(std::strcmp(point_owner(kBranchSiteAddr), "patch:br") == 0);
}

// 原语返回的 thunk 超出站点 ±128MB → 明确失败、撤回已写、不占点。
void test_install_branch_out_of_range() {
    reset_all();
    CHECK(arm_branch_site());
    g_prims.install_branch_thunk_delta = 0x10000000;  // 越界
    const ResourceClaim claim = make_patch_claim("patch:br", "P_TEST_BRANCH", &g_dummy_handler);
    CHECK(error_is(install_claim(claim, kBranchSiteAddr), "branch target out of BL range"));
    CHECK(g_prims.install_branch_calls == 1);
    CHECK(g_prims.revert_branch_calls == 1);  // 已写 B 被撤回
    CHECK(point_owner(kBranchSiteAddr) == nullptr);
}

// 原语分配 thunk 失败（返回 0）→ primitive rejected，不占点。
void test_install_branch_primitive_failure() {
    reset_all();
    CHECK(arm_branch_site());
    g_prims.install_branch_ok = false;
    const ResourceClaim claim = make_patch_claim("patch:br", "P_TEST_BRANCH", &g_dummy_handler);
    CHECK(error_is(install_claim(claim, kBranchSiteAddr), "primitive rejected"));
    CHECK(g_prims.install_branch_calls == 1);
    CHECK(point_owner(kBranchSiteAddr) == nullptr);
}

// 归还：写回原字并释放占用。
void test_release_branch_writes_back_and_releases() {
    reset_all();
    CHECK(arm_branch_site());
    const ResourceClaim claim = make_patch_claim("patch:br", "P_TEST_BRANCH", &g_dummy_handler);
    CHECK(install_claim(claim, kBranchSiteAddr).ok == 0);
    const Outcome released = release_claim(claim, kBranchSiteAddr);
    CHECK(released.ok == 0);
    CHECK(released.error == nullptr);
    CHECK(g_prims.revert_branch_calls == 1);
    CHECK(g_prims.last_addr == kBranchSiteAddr);
    CHECK(g_prims.last_expected == kBranchExpectedWord);
    CHECK(point_owner(kBranchSiteAddr) == nullptr);

    // 原语拒绝还原：保留占用（§4.2 tombstone 语义）。
    const ResourceClaim again = make_patch_claim("patch:br2", "P_TEST_BRANCH", &g_dummy_handler);
    CHECK(install_claim(again, kBranchSiteAddr).ok == 0);
    g_prims.revert_branch_ok = false;
    CHECK(error_is(release_claim(again, kBranchSiteAddr), "primitive rejected"));
    CHECK(point_owner(kBranchSiteAddr) != nullptr);
}

// kind/handler 不匹配：B 站点缺 handler → 拒绝，不调用原语、不占点。
void test_branch_handler_mismatch() {
    reset_all();
    CHECK(arm_branch_site());
    const ResourceClaim no_handler = make_patch_claim("patch:br", "P_TEST_BRANCH", nullptr);
    CHECK(error_is(install_claim(no_handler, kBranchSiteAddr), "kind mismatch"));
    CHECK(g_prims.install_branch_calls == 0);
    CHECK(point_owner(kBranchSiteAddr) == nullptr);

    // 字替换点带 handler → 拒绝（既有语义回归）。
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

// ---- kPtrSlot 指针槽机制 ----

void test_ptr_slot_install_success() {
    reset_all();
    void* expected = reinterpret_cast<void*>(0x1111);
    g_ptr_slot_storage = expected;
    const ResourceClaim claim = make_ptr_slot_claim("ptrslot:a", &g_dummy_handler);
    const Outcome outcome = install_claim(claim, ptr_site(expected));
    CHECK(outcome.ok == 0);
    CHECK(outcome.error == nullptr);
    CHECK(g_ptr_prims.write_calls == 1);
    CHECK(g_ptr_prims.last_addr == reinterpret_cast<uintptr_t>(&g_ptr_slot_storage));
    CHECK(g_ptr_prims.last_value == &g_dummy_handler);
    CHECK(g_ptr_slot_storage == &g_dummy_handler);
    CHECK(std::strcmp(point_owner(reinterpret_cast<uintptr_t>(&g_ptr_slot_storage)), "ptrslot:a") ==
          0);
}

void test_ptr_slot_site_and_handler_errors() {
    reset_all();
    void* expected = reinterpret_cast<void*>(0x2222);
    g_ptr_slot_storage = expected;
    const ResourceClaim claim = make_ptr_slot_claim("ptrslot:a", &g_dummy_handler);

    PtrSlotSite empty{};
    CHECK(error_is(install_claim(claim, empty), "slot not found"));
    CHECK(error_is(release_claim(claim, empty), "slot not found"));

    const ResourceClaim no_handler = make_ptr_slot_claim("ptrslot:b", nullptr);
    CHECK(error_is(install_claim(no_handler, ptr_site(expected)), "handler missing"));
    CHECK(g_ptr_prims.write_calls == 0);
    CHECK(claimed_count() == 0);
}

void test_ptr_slot_value_mismatch() {
    reset_all();
    const void* expected = reinterpret_cast<void*>(0x3333);
    g_ptr_slot_storage = reinterpret_cast<void*>(0x4444);  // 槽值 != expected
    const ResourceClaim claim = make_ptr_slot_claim("ptrslot:a", &g_dummy_handler);
    CHECK(error_is(install_claim(claim, ptr_site(const_cast<void*>(expected))),
                   "slot value mismatch"));
    CHECK(g_ptr_prims.write_calls == 0);
    CHECK(claimed_count() == 0);
    CHECK(g_ptr_slot_storage == reinterpret_cast<void*>(0x4444));
}

void test_ptr_slot_point_already_claimed() {
    reset_all();
    void* expected = reinterpret_cast<void*>(0x5555);
    g_ptr_slot_storage = expected;
    const ResourceClaim first = make_ptr_slot_claim("ptrslot:a", &g_dummy_handler);
    CHECK(install_claim(first, ptr_site(expected)).ok == 0);
    CHECK(g_ptr_slot_storage == &g_dummy_handler);

    // 第二个 claim 观察到当前值即其 expected（绕开值校验），占用冲突拒绝。
    const ResourceClaim second = make_ptr_slot_claim("ptrslot:b", &g_other_handler);
    CHECK(error_is(install_claim(second, ptr_site(&g_dummy_handler)), "point already claimed"));
    CHECK(g_ptr_prims.write_calls == 1);
    CHECK(std::strcmp(point_owner(reinterpret_cast<uintptr_t>(&g_ptr_slot_storage)), "ptrslot:a") ==
          0);
}

void test_ptr_slot_install_idempotent() {
    reset_all();
    void* expected = reinterpret_cast<void*>(0x6666);
    g_ptr_slot_storage = expected;
    const ResourceClaim claim = make_ptr_slot_claim("ptrslot:a", &g_dummy_handler);
    CHECK(install_claim(claim, ptr_site(expected)).ok == 0);
    // 同 claim 重复安装：值已是 handler → 幂等，不重复写。
    CHECK(install_claim(claim, ptr_site(expected)).ok == 0);
    CHECK(g_ptr_prims.write_calls == 1);
}

void test_ptr_slot_release_success_and_idempotent() {
    reset_all();
    void* expected = reinterpret_cast<void*>(0x7777);
    g_ptr_slot_storage = expected;
    const ResourceClaim claim = make_ptr_slot_claim("ptrslot:a", &g_dummy_handler);
    CHECK(install_claim(claim, ptr_site(expected)).ok == 0);

    const Outcome released = release_claim(claim, ptr_site(expected));
    CHECK(released.ok == 0);
    CHECK(g_ptr_prims.restore_calls == 1);
    CHECK(g_ptr_prims.last_value == expected);
    CHECK(g_ptr_slot_storage == expected);
    CHECK(point_owner(reinterpret_cast<uintptr_t>(&g_ptr_slot_storage)) == nullptr);

    // 槽已是原值：重复归还幂等成功，不重复写。
    CHECK(release_claim(claim, ptr_site(expected)).ok == 0);
    CHECK(g_ptr_prims.restore_calls == 1);
}

void test_ptr_slot_release_failure_keeps_owner() {
    reset_all();
    void* expected = reinterpret_cast<void*>(0x8888);
    g_ptr_slot_storage = expected;
    const ResourceClaim claim = make_ptr_slot_claim("ptrslot:a", &g_dummy_handler);
    CHECK(install_claim(claim, ptr_site(expected)).ok == 0);

    // 物理还原失败：保留占用与旧值（§4.2 tombstone 语义），槽不得被新 owner 覆盖。
    g_ptr_prims.restore_ok = false;
    CHECK(error_is(release_claim(claim, ptr_site(expected)), "primitive rejected"));
    CHECK(g_ptr_slot_storage == &g_dummy_handler);
    CHECK(std::strcmp(point_owner(reinterpret_cast<uintptr_t>(&g_ptr_slot_storage)), "ptrslot:a") ==
          0);
    const ResourceClaim other = make_ptr_slot_claim("ptrslot:b", &g_other_handler);
    CHECK(error_is(install_claim(other, ptr_site(&g_dummy_handler)), "point already claimed"));

    // 原语恢复后重试归还：写回原值、释放占用，槽可被重新声明。
    g_ptr_prims.restore_ok = true;
    CHECK(release_claim(claim, ptr_site(expected)).ok == 0);
    CHECK(g_ptr_slot_storage == expected);
    CHECK(point_owner(reinterpret_cast<uintptr_t>(&g_ptr_slot_storage)) == nullptr);
    CHECK(install_claim(other, ptr_site(expected)).ok == 0);
    CHECK(g_ptr_slot_storage == &g_other_handler);
}

}  // namespace

int main() {
    test_primitives_not_set();  // 必须在任何 set_primitives 之前
    test_claims_arbitration();
    test_claims_capacity();
    test_install_bl_success();
    test_install_word_success();
    test_bl_redirect_to_symbol_success();
    test_bl_redirect_to_symbol_mismatch();
    test_bl_redirect_to_symbol_range_and_alignment();
    test_install_word_at();
    test_kind_mismatch();
    test_install_branch_success();
    test_install_branch_out_of_range();
    test_install_branch_primitive_failure();
    test_release_branch_writes_back_and_releases();
    test_branch_handler_mismatch();
    test_site_and_address_errors();
    test_point_already_claimed();
    test_primitive_rejected_releases_claim();
    test_release_success();
    test_release_primitive_rejected_keeps_owner();
    test_ptr_slot_install_success();
    test_ptr_slot_site_and_handler_errors();
    test_ptr_slot_value_mismatch();
    test_ptr_slot_point_already_claimed();
    test_ptr_slot_install_idempotent();
    test_ptr_slot_release_success_and_idempotent();
    test_ptr_slot_release_failure_keeps_owner();
    std::printf("patch_layer: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
