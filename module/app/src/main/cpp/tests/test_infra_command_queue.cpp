// infra/command_queue.h + infra/runtime.h host 单测：
// 覆盖 FIFO/票据终态、非法入队、取消语义、容量上限、多线程 post、reset 与帧时钟注入。

#include "infra/command_queue.h"
#include "infra/test_reset.h"
#include "infra/runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::infra::cancel;
using qol::infra::Command;
using qol::infra::CommandState;
using qol::infra::CommandTicket;
using qol::infra::drain;
using qol::infra::pending_count;
using qol::infra::post;
using qol::infra::reset_for_test;
using qol::infra::ticket_state;

namespace {

// 执行记录：run 被调用时把 ctx 指向的 tag 追加进来，用于验证顺序与是否执行。
std::vector<int> g_order;

bool run_tag(void* ctx) {
    g_order.push_back(*static_cast<int*>(ctx));
    return true;
}

bool run_fail(void* ctx) {
    g_order.push_back(*static_cast<int*>(ctx));
    return false;
}

// 执行中自我取消：run 内调用 cancel(self)，应失败（票据已开始）。
struct SelfCancelCtx {
    CommandTicket ticket = 0;
    bool cancel_result = true;
};

bool run_self_cancel(void* ctx) {
    auto* c = static_cast<SelfCancelCtx*>(ctx);
    c->cancel_result = cancel(c->ticket);
    return true;
}

// 1. 基本 post → drain：FIFO 顺序、返回条数、终态映射。
void test_basic_post_drain() {
    reset_for_test();
    g_order.clear();

    int a = 1, b = 2, c = 3;
    const Command ca{"a", run_tag, &a, false};
    const Command cb{"b", run_fail, &b, false};
    const Command cc{"c", run_tag, &c, false};
    const CommandTicket ta = post(ca);
    const CommandTicket tb = post(cb);
    const CommandTicket tc = post(cc);

    CHECK(ta == 1);  // 票据从 1 起单调递增
    CHECK(tb == ta + 1);
    CHECK(tc == tb + 1);
    CHECK(pending_count() == 3);
    CHECK(ticket_state(ta) == CommandState::kPending);

    CHECK(drain() == 3);
    CHECK(g_order.size() == 3 && g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3);  // FIFO
    CHECK(ticket_state(ta) == CommandState::kDone);
    CHECK(ticket_state(tb) == CommandState::kFailed);  // run 返回 false
    CHECK(ticket_state(tc) == CommandState::kDone);
    CHECK(pending_count() == 0);
    CHECK(drain() == 0);  // 空队列返回 0
}

// 2. 未知票据 → kFailed；非法命令（nullptr）→ 票据 0。
void test_unknown_and_illegal() {
    reset_for_test();
    g_order.clear();

    CHECK(ticket_state(0) == CommandState::kFailed);
    CHECK(ticket_state(123456) == CommandState::kFailed);
    CHECK(!cancel(0));
    CHECK(!cancel(123456));

    int v = 0;
    const Command null_run{"null_run", nullptr, &v, false};
    const Command null_name{nullptr, run_tag, &v, false};
    CHECK(post(null_run) == 0);
    CHECK(post(null_name) == 0);
    CHECK(pending_count() == 0);
    CHECK(drain() == 0);
    CHECK(g_order.empty());
}

// 3. cancel：可取消未执行成功且不被 drain 执行；不可取消失败；已执行后失败；执行中失败。
void test_cancel() {
    reset_for_test();
    g_order.clear();

    int a = 1;
    const Command ca{"a", run_tag, &a, true};  // 可取消
    const CommandTicket ta = post(ca);
    CHECK(cancel(ta));
    CHECK(ticket_state(ta) == CommandState::kCancelled);
    CHECK(drain() == 0);
    CHECK(g_order.empty());
    CHECK(!cancel(ta));  // 已取消后不可再次取消

    int b = 2;
    const Command cb{"b", run_tag, &b, false};  // 不可取消
    const CommandTicket tb = post(cb);
    CHECK(!cancel(tb));
    CHECK(ticket_state(tb) == CommandState::kPending);
    CHECK(drain() == 1);
    CHECK(ticket_state(tb) == CommandState::kDone);
    CHECK(!cancel(tb));  // 已执行后取消失败

    int c = 3;
    const Command cc{"c", run_tag, &c, true};
    const CommandTicket tc = post(cc);
    CHECK(drain() == 1);
    CHECK(ticket_state(tc) == CommandState::kDone);
    CHECK(!cancel(tc));  // 已执行后取消失败

    SelfCancelCtx self;
    const Command cs{"self", run_self_cancel, &self, true};
    self.ticket = post(cs);
    CHECK(drain() == 1);
    CHECK(!self.cancel_result);  // 执行中取消必须失败（票据已开始）
    CHECK(ticket_state(self.ticket) == CommandState::kDone);
}

// 4. 队列满：超过容量返回 0，已入队不受影响；终态窗口淘汰旧票据。
void test_capacity_and_eviction() {
    reset_for_test();
    g_order.clear();

    static std::vector<int> tags(1025);  // 稳定地址，供 ctx 指针使用
    const int kCap = 1024;
    CommandTicket first = 0;
    for (int i = 0; i < kCap; ++i) {
        tags[i] = i;
        const Command c{"fill", run_tag, &tags[i], false};
        const CommandTicket t = post(c);
        if (t == 0) {
            CHECK(false);
        }
        if (i == 0) first = t;
    }
    CHECK(pending_count() == static_cast<uint32_t>(kCap));

    tags[kCap] = kCap;
    const Command over{"over", run_tag, &tags[kCap], false};
    CHECK(post(over) == 0);  // 满 → 0
    CHECK(pending_count() == static_cast<uint32_t>(kCap));  // 已入队不受影响

    CHECK(drain() == static_cast<uint32_t>(kCap));
    CHECK(ticket_state(first) == CommandState::kDone);

    // 再入队并执行第 1025 条终态：最早的票据被有界窗口淘汰，查询按未知处理。
    const Command extra{"extra", run_tag, &tags[kCap], false};
    const CommandTicket tx = post(extra);
    CHECK(tx != 0);
    CHECK(drain() == 1);
    CHECK(ticket_state(tx) == CommandState::kDone);
    CHECK(ticket_state(first) == CommandState::kFailed);  // 已淘汰
}

// 5. 多线程 post：2 线程 × 各 200 条，一次 drain 执行 400 条且票据无重复。
void test_multithread_post() {
    reset_for_test();
    g_order.clear();

    constexpr int kPerThread = 200;
    constexpr int kThreads = 2;
    std::vector<int> tags(kThreads * kPerThread);
    std::vector<CommandTicket> tickets[kThreads];
    for (int i = 0; i < kThreads; ++i) tickets[i].reserve(kPerThread);

    std::atomic<bool> start{false};
    auto worker = [&](int idx) {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kPerThread; ++i) {
            const int k = idx * kPerThread + i;
            tags[k] = k;
            const Command c{"mt", run_tag, &tags[k], false};
            const CommandTicket t = post(c);
            if (t != 0) tickets[idx].push_back(t);
        }
    };

    std::thread t0(worker, 0);
    std::thread t1(worker, 1);
    start.store(true, std::memory_order_release);
    t0.join();
    t1.join();

    CHECK(tickets[0].size() == static_cast<std::size_t>(kPerThread));
    CHECK(tickets[1].size() == static_cast<std::size_t>(kPerThread));
    CHECK(pending_count() == static_cast<uint32_t>(kThreads * kPerThread));

    CHECK(drain() == static_cast<uint32_t>(kThreads * kPerThread));
    CHECK(g_order.size() == static_cast<std::size_t>(kThreads * kPerThread));

    // 两线程分配的票据互不重复。
    std::unordered_set<CommandTicket> all;
    for (int i = 0; i < kThreads; ++i) {
        for (const CommandTicket t : tickets[i]) all.insert(t);
    }
    CHECK(all.size() == static_cast<std::size_t>(kThreads * kPerThread));
}

// 6. reset_for_test 清空 pending 与票据状态，票据计数重置。
void test_reset_for_test() {
    reset_for_test();
    g_order.clear();

    int a = 1;
    const Command c{"a", run_tag, &a, true};
    const CommandTicket t = post(c);
    CHECK(t == 1);
    CHECK(pending_count() == 1);

    reset_for_test();
    CHECK(pending_count() == 0);
    CHECK(ticket_state(t) == CommandState::kFailed);  // 终态与活跃表都清空

    // 计数重置后新票据重新从 1 开始。
    const CommandTicket t2 = post(c);
    CHECK(t2 == 1);
    reset_for_test();
}

// 7. runtime：未注入返回 -1；注入假时钟后转发；覆盖写入以最后一次为准。
int64_t fake_clock_a() { return 12345; }
int64_t fake_clock_b() { return -7; }

void test_runtime_clock() {
    CHECK(qol::infra::frame_count() == -1);  // 未注入
    qol::infra::runtime_init(qol::infra::Clock{&fake_clock_a});
    CHECK(qol::infra::frame_count() == 12345);
    qol::infra::runtime_init(qol::infra::Clock{&fake_clock_b});  // 覆盖写入
    CHECK(qol::infra::frame_count() == -7);
    qol::infra::runtime_init(qol::infra::Clock{nullptr});
    CHECK(qol::infra::frame_count() == -1);  // nullptr 注入视为未就绪
}

}  // namespace

int main() {
    test_basic_post_drain();
    test_unknown_and_illegal();
    test_cancel();
    test_capacity_and_eviction();
    test_multithread_post();
    test_reset_for_test();
    test_runtime_clock();
    std::printf("infra_command_queue: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
