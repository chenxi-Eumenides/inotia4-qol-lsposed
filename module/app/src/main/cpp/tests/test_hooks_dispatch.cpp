// hooks/dispatch.h host 单测（ARCHITECTURE.md §4.5，v0.3.8）：
// before 阶段注册顺序 / after 阶段逆序、每个回调每次分派只调用一次、Modify 链式传递、
// Observe 返回值忽略、Intercept 跨阶段唯一与短路（短路后 after 仅 Observe）、
// 快照语义、容量 32、未注册资源与非法参数、重入。

#include "hooks/dispatch.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::hooks::ConsumerKind;
using qol::hooks::dispatch_consumer_count;
using qol::hooks::dispatch_reset_for_test;
using qol::hooks::dispatch_run_text;
using qol::hooks::dispatch_subscribe;
using qol::hooks::dispatch_unsubscribe;
using qol::hooks::DispatchId;
using qol::hooks::DispatchPhase;
using qol::hooks::InterceptHandle;
using qol::hooks::ModifyHandle;
using qol::hooks::ObserveHandle;

namespace {

constexpr const char* kRes = "hook:test:text";
constexpr const char* kResB = "hook:test:other";

constexpr DispatchPhase kBefore = DispatchPhase::kBefore;
constexpr DispatchPhase kAfter = DispatchPhase::kAfter;

char g_order[64] = {};
int g_order_n = 0;
int g_obs_a = 0, g_obs_b = 0, g_mod_m = 0, g_mod_n = 0, g_mod_null = 0, g_ic_i = 0, g_ic_null = 0;

void note(char c) {
    if (g_order_n < 63) g_order[g_order_n++] = c;
    g_order[g_order_n] = '\0';
}

void reset_trace() {
    g_order[0] = '\0';
    g_order_n = 0;
    g_obs_a = g_obs_b = g_mod_m = g_mod_n = g_mod_null = g_ic_i = g_ic_null = 0;
}

const char* obs_a(uint32_t, const char* cur, void*) {
    ++g_obs_a;
    note('a');
    (void)cur;
    return "IGNORED";  // Observe 返回值必须被忽略
}
const char* obs_b(uint32_t, const char* cur, void*) {
    ++g_obs_b;
    note('b');
    (void)cur;
    return "IGNORED";
}
const char* mod_m(uint32_t, const char* cur, void*) {
    ++g_mod_m;
    note('m');
    (void)cur;
    return "M";
}
const char* mod_n(uint32_t, const char* cur, void*) {
    ++g_mod_n;
    note('n');
    (void)cur;
    return "N";
}
const char* mod_null(uint32_t, const char* cur, void*) {
    ++g_mod_null;
    note('x');
    (void)cur;
    return nullptr;  // 不改
}
const char* ic_i(uint32_t, const char* cur, void*) {
    ++g_ic_i;
    note('i');
    (void)cur;
    return "I";
}
const char* ic_null(uint32_t, const char* cur, void*) {
    ++g_ic_null;
    note('i');
    (void)cur;
    return nullptr;  // 回退当前值
}

// 快照语义：在回调内注销另一个消费者。
DispatchId g_victim_id = 0;
const char* obs_cancel_victim(uint32_t, const char* cur, void*) {
    note('v');
    (void)cur;
    dispatch_unsubscribe(kRes, g_victim_id);
    return nullptr;
}
const char* obs_victim(uint32_t, const char* cur, void*) {
    note('w');
    (void)cur;
    return nullptr;
}

void test_unregistered_and_invalid() {
    dispatch_reset_for_test();
    CHECK(std::strcmp(dispatch_run_text(kRes, 1, "O"), "O") == 0);
    CHECK(std::strcmp(dispatch_run_text(nullptr, 1, "O"), "O") == 0);  // 透传 original
    CHECK(std::strcmp(dispatch_run_text("", 1, "O"), "O") == 0);
    CHECK(dispatch_subscribe(nullptr, kBefore, ConsumerKind::kObserve, obs_a, nullptr) == 0);
    CHECK(dispatch_subscribe("", kBefore, ConsumerKind::kObserve, obs_a, nullptr) == 0);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, nullptr, nullptr) == 0);
    CHECK(dispatch_subscribe(kRes, static_cast<DispatchPhase>(9), ConsumerKind::kObserve, obs_a,
                             nullptr) == 0);
    CHECK(dispatch_subscribe(kRes, kBefore, static_cast<ConsumerKind>(9), obs_a, nullptr) == 0);
    CHECK(dispatch_unsubscribe(kRes, 0) == false);
    CHECK(dispatch_unsubscribe(kRes, 12345) == false);
    CHECK(dispatch_unsubscribe(nullptr, 1) == false);
    CHECK(dispatch_consumer_count(kRes, kBefore, ConsumerKind::kObserve) == 0);
}

void test_before_order_and_after_reverse_single_invocation() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_a, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kModify, mod_m, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kObserve, obs_b, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kModify, mod_n, nullptr) != 0);
    // before（注册顺序）：a, m(M)；after（逆序）：n(N), b。
    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "N") == 0);
    CHECK(std::strcmp(g_order, "amnb") == 0);
    // 每个回调恰好调用一次。
    CHECK(g_obs_a == 1 && g_mod_m == 1 && g_obs_b == 1 && g_mod_n == 1);
}

void test_before_modify_chain() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kModify, mod_m, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kModify, mod_n, nullptr) != 0);
    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "N") == 0);  // 后注册者累积到最终
    CHECK(std::strcmp(g_order, "mn") == 0);
    CHECK(g_mod_m == 1 && g_mod_n == 1);
}

void test_after_modify_reverse_chain() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kModify, mod_m, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kModify, mod_n, nullptr) != 0);
    // 逆序：n(N) 先、m(M) 后 → 最终 M；各一次。
    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "M") == 0);
    CHECK(std::strcmp(g_order, "nm") == 0);
    CHECK(g_mod_m == 1 && g_mod_n == 1);
}

void test_observe_ignored() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_a, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kObserve, obs_b, nullptr) != 0);
    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "O") == 0);
    CHECK(std::strcmp(g_order, "ab") == 0);
    CHECK(g_obs_a == 1 && g_obs_b == 1);
}

void test_modify_null_keeps_current() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kModify, mod_null, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kModify, mod_m, nullptr) != 0);
    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "M") == 0);
    CHECK(std::strcmp(g_order, "xm") == 0);
    CHECK(g_mod_null == 1 && g_mod_m == 1);
}

void test_intercept_before_short_circuit() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_a, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kModify, mod_m, nullptr) != 0);
    const InterceptHandle ic =
        dispatch_subscribe(kRes, kBefore, ConsumerKind::kIntercept, ic_i, nullptr);
    CHECK(ic != 0);
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kObserve, obs_b, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kModify, mod_n, nullptr) != 0);

    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "I") == 0);
    // before：a,m,i 命中短路；after Observe b 仍执行；after Modify n 被跳过。
    CHECK(std::strcmp(g_order, "amib") == 0);
    CHECK(g_obs_a == 1 && g_mod_m == 1 && g_ic_i == 1 && g_obs_b == 1 && g_mod_n == 0);
}

void test_intercept_after_short_circuit() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_a, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kModify, mod_m, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kModify, mod_n, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kObserve, obs_b, nullptr) != 0);
    const InterceptHandle ic =
        dispatch_subscribe(kRes, kAfter, ConsumerKind::kIntercept, ic_i, nullptr);
    CHECK(ic != 0);

    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "I") == 0);
    // before：a,m；after 逆序：i 命中短路，b（残余 Observe）仍执行，n（Modify）跳过。
    CHECK(std::strcmp(g_order, "amib") == 0);
    CHECK(g_obs_a == 1 && g_mod_m == 1 && g_ic_i == 1 && g_obs_b == 1 && g_mod_n == 0);
}

void test_intercept_unique_across_phases() {
    dispatch_reset_for_test();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kIntercept, ic_i, nullptr) != 0);
    // 跨阶段唯一：after 阶段第二个 Intercept 也拒绝。
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kIntercept, ic_i, nullptr) == 0);
    CHECK(dispatch_consumer_count(kRes, kBefore, ConsumerKind::kIntercept) == 1);
    CHECK(dispatch_consumer_count(kRes, kAfter, ConsumerKind::kIntercept) == 0);

    // 反向：先 after 后 before。
    dispatch_reset_for_test();
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kIntercept, ic_i, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kIntercept, ic_i, nullptr) == 0);

    // 注销后可重新注册。
    dispatch_reset_for_test();
    const InterceptHandle h = dispatch_subscribe(kRes, kAfter, ConsumerKind::kIntercept, ic_i, nullptr);
    CHECK(h != 0);
    CHECK(dispatch_unsubscribe(kRes, h));
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kIntercept, ic_i, nullptr) != 0);
}

void test_intercept_null_falls_back() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kModify, mod_m, nullptr) != 0);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kIntercept, ic_null, nullptr) != 0);
    // 短路但返回 nullptr → 回退当前值（before Modify 的结果 M）。
    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "M") == 0);
    CHECK(std::strcmp(g_order, "mi") == 0);
    CHECK(g_mod_m == 1 && g_ic_null == 1);
}

void test_snapshot_semantics() {
    dispatch_reset_for_test();
    reset_trace();
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_cancel_victim, nullptr) != 0);
    g_victim_id = dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_victim, nullptr);
    CHECK(g_victim_id != 0);
    // 本轮快照仍包含 victim（注销下轮生效）。
    dispatch_run_text(kRes, 7, "O");
    CHECK(std::strcmp(g_order, "vw") == 0);
    CHECK(dispatch_consumer_count(kRes, kBefore, ConsumerKind::kObserve) == 1);
    // 下一轮只剩 cancel_victim。
    reset_trace();
    dispatch_run_text(kRes, 7, "O");
    CHECK(std::strcmp(g_order, "v") == 0);
}

void test_capacity_and_count() {
    dispatch_reset_for_test();
    for (int i = 0; i < 32; ++i) {
        CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_a, nullptr) != 0);
    }
    CHECK(dispatch_consumer_count(kRes, kBefore, ConsumerKind::kObserve) == 32);
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_a, nullptr) == 0);
    // 其它资源独立容量。
    CHECK(dispatch_subscribe(kResB, kAfter, ConsumerKind::kModify, mod_m, nullptr) != 0);
    CHECK(dispatch_consumer_count(kResB, kAfter, ConsumerKind::kModify) == 1);
    CHECK(dispatch_consumer_count(kRes, kAfter, ConsumerKind::kModify) == 0);
    // phase 分组计数。
    CHECK(dispatch_consumer_count(kRes, kBefore, ConsumerKind::kModify) == 0);
}

// 方案 X（§4.6 登记表即声明式消费者）：owner 内置表最先注册为 after+Modify，
// before 阶段消费者应看到**原函数文本**（非 owner 表文本）；owner 表在 after 逆序链
// 最后执行，覆盖第三方 Modify 的结果。
const char* g_probe_seen = nullptr;
const char* g_owner_seen = nullptr;
int g_probe_calls = 0, g_third_calls = 0, g_owner_calls = 0;

const char* before_probe_cb(uint32_t, const char* cur, void*) {
    ++g_probe_calls;
    g_probe_seen = cur;
    return "IGNORED";
}
const char* third_mod_cb(uint32_t, const char* cur, void*) {
    (void)cur;
    ++g_third_calls;
    return "THIRD";
}
const char* owner_mod_cb(uint32_t, const char* cur, void*) {
    ++g_owner_calls;
    g_owner_seen = cur;
    return "TABLE";
}

void test_owner_table_after_precedence() {
    dispatch_reset_for_test();
    g_probe_seen = nullptr;
    g_owner_seen = nullptr;
    g_probe_calls = g_third_calls = g_owner_calls = 0;
    // owner 内置表最先注册（after+Modify）。
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kModify, owner_mod_cb, nullptr) != 0);
    // before 观察者。
    CHECK(dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, before_probe_cb, nullptr) != 0);
    // 第三方 Modify 后注册（after；逆序先执行）。
    CHECK(dispatch_subscribe(kRes, kAfter, ConsumerKind::kModify, third_mod_cb, nullptr) != 0);

    CHECK(std::strcmp(dispatch_run_text(kRes, 35185, "GAME"), "TABLE") == 0);
    // before 看到原函数文本，而非 owner 表文本。
    CHECK(g_probe_seen != nullptr && std::strcmp(g_probe_seen, "GAME") == 0);
    // owner 在 after 链最后执行，看到第三方结果并被覆盖。
    CHECK(g_owner_seen != nullptr && std::strcmp(g_owner_seen, "THIRD") == 0);
    CHECK(g_probe_calls == 1 && g_third_calls == 1 && g_owner_calls == 1);
}

void test_count_unsubscribe_reset() {
    dispatch_reset_for_test();
    const ObserveHandle o = dispatch_subscribe(kRes, kBefore, ConsumerKind::kObserve, obs_a, nullptr);
    const ModifyHandle m = dispatch_subscribe(kRes, kAfter, ConsumerKind::kModify, mod_m, nullptr);
    CHECK(o != 0 && m != 0);
    CHECK(dispatch_consumer_count(kRes, kBefore, ConsumerKind::kObserve) == 1);
    CHECK(dispatch_consumer_count(kRes, kAfter, ConsumerKind::kModify) == 1);
    CHECK(dispatch_unsubscribe(kRes, o));
    CHECK(!dispatch_unsubscribe(kRes, o));  // 已注销
    CHECK(dispatch_consumer_count(kRes, kBefore, ConsumerKind::kObserve) == 0);
    dispatch_reset_for_test();
    CHECK(dispatch_consumer_count(kRes, kAfter, ConsumerKind::kModify) == 0);
    CHECK(std::strcmp(dispatch_run_text(kRes, 7, "O"), "O") == 0);
}

}  // namespace

int main() {
    test_unregistered_and_invalid();
    test_before_order_and_after_reverse_single_invocation();
    test_before_modify_chain();
    test_after_modify_reverse_chain();
    test_observe_ignored();
    test_modify_null_keeps_current();
    test_intercept_before_short_circuit();
    test_intercept_after_short_circuit();
    test_intercept_unique_across_phases();
    test_intercept_null_falls_back();
    test_snapshot_semantics();
    test_capacity_and_count();
    test_owner_table_after_precedence();
    test_count_unsubscribe_reset();
    std::printf("hooks_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
