// infra/events.h host 单测：订阅顺序、类型隔离、注销幂等、容量上限、快照派发语义。
// 编译方式见 tests/CMakeLists.txt 的 infra_tests 目标。

#include "infra/events.h"
#include "infra/test_reset.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::infra::publish;
using qol::infra::subscribe;
using qol::infra::SubscriptionId;

// 调用记录：tag 标识处理器，payload 原样回传用于验证透传。
std::vector<int> g_order;
std::vector<const void*> g_payloads;

struct HandlerCtx {
    int tag = 0;
    SubscriptionId self_id = 0;               // 非 0 时在处理器内注销自身
    const SubscriptionId* target = nullptr;   // 指向待注销 id，调用时读取
};

void record(void* ctx, const void* payload) {
    auto* c = static_cast<HandlerCtx*>(ctx);
    g_order.push_back(c->tag);
    g_payloads.push_back(payload);
}

// 先记录调用，再按 ctx 指令注销，用于验证派发中注销的快照语义。
void record_and_unsubscribe(void* ctx, const void* payload) {
    auto* c = static_cast<HandlerCtx*>(ctx);
    g_order.push_back(c->tag);
    g_payloads.push_back(payload);
    if (c->self_id != 0) {
        qol::infra::unsubscribe(c->self_id);
    }
    if (c->target != nullptr && *c->target != 0) {
        qol::infra::unsubscribe(*c->target);
    }
}

void reset_log() {
    g_order.clear();
    g_payloads.clear();
}

void test_subscription_order_and_payload() {
    qol::infra::reset_for_test();
    reset_log();

    HandlerCtx a{1};
    HandlerCtx b{2};
    HandlerCtx c{3};
    const SubscriptionId ida = subscribe(7, record, &a);
    const SubscriptionId idb = subscribe(7, record, &b);
    const SubscriptionId idc = subscribe(7, record, &c);
    CHECK(ida != 0 && idb != 0 && idc != 0);
    // SubscriptionId 从 1 起单调递增。
    CHECK(ida == 1);
    CHECK(idb == ida + 1);
    CHECK(idc == idb + 1);

    int payload = 42;
    CHECK(publish(7, &payload) == 3);
    CHECK(g_order.size() == 3);
    CHECK(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3);  // 按订阅顺序
    CHECK(g_payloads.size() == 3);
    CHECK(g_payloads[0] == &payload && g_payloads[1] == &payload && g_payloads[2] == &payload);
}

void test_type_isolation_and_empty_publish() {
    qol::infra::reset_for_test();
    reset_log();

    HandlerCtx a{1};
    HandlerCtx b{2};
    subscribe(10, record, &a);
    subscribe(20, record, &b);

    publish(10, nullptr);
    CHECK(g_order.size() == 1 && g_order[0] == 1);
    reset_log();
    publish(20, nullptr);
    CHECK(g_order.size() == 1 && g_order[0] == 2);

    // 无订阅的 type：返回 0，不调用任何处理器。
    reset_log();
    CHECK(publish(99, nullptr) == 0);
    CHECK(g_order.empty());
}

void test_unsubscribe() {
    qol::infra::reset_for_test();
    reset_log();

    HandlerCtx a{1};
    const SubscriptionId id = subscribe(5, record, &a);
    CHECK(id != 0);
    CHECK(qol::infra::subscription_count() == 1);

    CHECK(qol::infra::unsubscribe(id));
    CHECK(qol::infra::subscription_count() == 0);
    reset_log();
    CHECK(publish(5, nullptr) == 0);
    CHECK(g_order.empty());

    CHECK(!qol::infra::unsubscribe(id));          // 重复注销
    CHECK(!qol::infra::unsubscribe(123456));      // 未知 id
    CHECK(!qol::infra::unsubscribe(0));           // 0 为无效 id
}

// 快照语义：派发期间注销的订阅在本轮仍可能被调用一次，下一轮才消失。
void test_unsubscribe_during_dispatch_self() {
    qol::infra::reset_for_test();
    reset_log();

    HandlerCtx a{1};
    HandlerCtx b{2};
    a.self_id = subscribe(5, record_and_unsubscribe, &a);
    subscribe(5, record, &b);

    // 第一轮：a 注销自身，但快照中仍有 a 与 b，二者都被调用。
    CHECK(publish(5, nullptr) == 2);
    CHECK(g_order.size() == 2 && g_order[0] == 1 && g_order[1] == 2);
    CHECK(qol::infra::subscription_count() == 1);

    // 第二轮：a 已从订阅表移除，只剩 b。
    reset_log();
    CHECK(publish(5, nullptr) == 1);
    CHECK(g_order.size() == 1 && g_order[0] == 2);
}

void test_unsubscribe_during_dispatch_other() {
    qol::infra::reset_for_test();
    reset_log();

    // a 先订阅、b 后订阅，保证本轮派发顺序为 a → b；a 在处理器内注销 b。
    HandlerCtx a{1};
    HandlerCtx b{2};
    SubscriptionId idb = 0;
    a.target = &idb;
    subscribe(6, record_and_unsubscribe, &a);
    idb = subscribe(6, record_and_unsubscribe, &b);

    // b 虽已被 a 注销，本轮快照仍包含 b，故仍被调用一次。
    CHECK(publish(6, nullptr) == 2);
    CHECK(g_order.size() == 2 && g_order[0] == 1 && g_order[1] == 2);
    CHECK(qol::infra::subscription_count() == 1);

    reset_log();
    CHECK(publish(6, nullptr) == 1);
    CHECK(g_order.size() == 1 && g_order[0] == 1);
}

void test_capacity_and_null_handler() {
    qol::infra::reset_for_test();
    reset_log();

    static std::vector<HandlerCtx> holders(257);  // 稳定地址，供 ctx 指针使用
    for (int i = 0; i < 256; ++i) {
        holders[i].tag = i;
        CHECK(subscribe(1, record, &holders[i]) != 0);
    }
    CHECK(qol::infra::subscription_count() == 256);

    // 第 257 条：表满，返回 0。
    holders[256].tag = 256;
    CHECK(subscribe(1, record, &holders[256]) == 0);
    CHECK(qol::infra::subscription_count() == 256);

    // handler == nullptr：无论是否满都返回 0。
    CHECK(subscribe(1, nullptr, nullptr) == 0);
    CHECK(qol::infra::subscription_count() == 256);

    // 注销一条后腾出容量，可再次订阅成功。
    CHECK(qol::infra::unsubscribe(1));
    CHECK(subscribe(2, record, &holders[256]) != 0);
    CHECK(qol::infra::subscription_count() == 256);
}

void test_clear_all() {
    qol::infra::reset_for_test();
    reset_log();

    HandlerCtx a{1};
    HandlerCtx b{2};
    subscribe(1, record, &a);
    subscribe(2, record, &b);
    CHECK(qol::infra::subscription_count() == 2);

    qol::infra::clear_all();
    CHECK(qol::infra::subscription_count() == 0);
    reset_log();
    CHECK(publish(1, nullptr) == 0);
    CHECK(publish(2, nullptr) == 0);
    CHECK(g_order.empty());

    // clear_all 同时重置 id 计数：新订阅重新从 1 开始。
    CHECK(subscribe(3, record, &a) == 1);

    // reset_for_test 与 clear_all 同义。
    qol::infra::reset_for_test();
    CHECK(qol::infra::subscription_count() == 0);
}

}  // namespace

int main() {
    test_subscription_order_and_payload();
    test_type_isolation_and_empty_publish();
    test_unsubscribe();
    test_unsubscribe_during_dispatch_self();
    test_unsubscribe_during_dispatch_other();
    test_capacity_and_null_handler();
    test_clear_all();
    std::printf("infra_events: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
