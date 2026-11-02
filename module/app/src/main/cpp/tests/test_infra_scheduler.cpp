// infra/scheduler.h host 单测：
// 覆盖注册/注销/容量/空 handler、interval 归一与 count 递减、按帧去重、
// handler 返回 false 自动注销、注册顺序派发、点位隔离、reset、未注入帧号跳过。

#include "infra/scheduler.h"
#include "infra/runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::infra::FramePoint;
using qol::infra::FrameTaskId;
using qol::infra::scheduler_add;
using qol::infra::scheduler_dispatch;
using qol::infra::scheduler_remove;
using qol::infra::scheduler_reset_for_test;
using qol::infra::scheduler_task_count;

namespace {

int64_t g_frame = -1;
int64_t fake_clock() { return g_frame; }

void reset_all(int64_t frame) {
    scheduler_reset_for_test();
    g_frame = frame;
    qol::infra::runtime_init(qol::infra::Clock{&fake_clock});
}

int g_calls = 0;
int64_t g_last_frame = 0;

bool counting_handler(int64_t frame, void* ctx) {
    (void)ctx;
    ++g_calls;
    g_last_frame = frame;
    return true;
}

bool once_handler(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    ++g_calls;
    return false;
}

char g_order[16] = {};
int g_order_n = 0;

void push_order(char c) {
    if (g_order_n < 15) g_order[g_order_n++] = c;
    g_order[g_order_n] = '\0';
}

bool order_a(int64_t, void*) { push_order('A'); return true; }
bool order_b(int64_t, void*) { push_order('B'); return true; }
bool order_c(int64_t, void*) { push_order('C'); return true; }
bool order_d(int64_t, void*) { push_order('D'); return true; }

int g_render_calls = 0;
int g_logic_calls = 0;
bool render_handler(int64_t, void*) { ++g_render_calls; return true; }
bool logic_handler(int64_t, void*) { ++g_logic_calls; return true; }

void test_add_remove_and_invalid() {
    reset_all(1);
    CHECK(scheduler_add(FramePoint::kRenderPre, nullptr, nullptr, 1, 0) == 0);
    CHECK(scheduler_add(static_cast<FramePoint>(7), counting_handler, nullptr, 1, 0) == 0);

    const FrameTaskId id = scheduler_add(FramePoint::kRenderPre, counting_handler, nullptr, 1, 0);
    CHECK(id != 0);
    CHECK(scheduler_task_count(FramePoint::kRenderPre) == 1);
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 0);

    CHECK(scheduler_remove(id));
    CHECK(!scheduler_remove(id));  // 已注销
    CHECK(!scheduler_remove(0));   // 无效 id
    CHECK(scheduler_task_count(FramePoint::kRenderPre) == 0);
}

void test_capacity_128() {
    reset_all(-1);
    FrameTaskId ids[128] = {};
    for (uint32_t i = 0; i < 128; ++i) {
        ids[i] = scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, 1, 0);
        CHECK(ids[i] != 0);
    }
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 128);
    CHECK(scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, 1, 0) == 0);  // 满
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 128);
    // 释放一个后可以再注册。
    CHECK(scheduler_remove(ids[64]));
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 127);
    CHECK(scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, 1, 0) != 0);
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 128);
}

void test_interval_and_frame_dedupe() {
    reset_all(-1);
    g_calls = 0;
    CHECK(scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, 3, 0) != 0);

    // 未注入帧号（-1）与帧号 0：跳过派发。
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 0);
    g_frame = 0;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 0);

    // 首个派发周期必触发；同一帧号不重复派发。
    g_frame = 1;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 1);
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 1);

    // interval=3：下一次到期为 1+3=4。
    g_frame = 2;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 1);
    g_frame = 3;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 1);
    g_frame = 4;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 2);
    CHECK(g_last_frame == 4);

    // interval 归一：0/负值视为 1（每帧）。
    reset_all(-1);
    g_calls = 0;
    CHECK(scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, 0, 0) != 0);
    g_frame = 1;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 1);
    g_frame = 2;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 2);

    reset_all(-1);
    g_calls = 0;
    CHECK(scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, -5, 0) != 0);
    g_frame = 1;
    scheduler_dispatch(FramePoint::kLogicPre);
    g_frame = 2;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 2);
}

void test_count_decrement_and_auto_remove() {
    reset_all(-1);
    g_calls = 0;
    CHECK(scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, 1, 2) != 0);

    g_frame = 1;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 1);
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 1);

    g_frame = 2;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 2);
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 0);  // count 耗尽自动注销

    g_frame = 3;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 2);
}

void test_handler_false_auto_remove() {
    reset_all(-1);
    g_calls = 0;
    CHECK(scheduler_add(FramePoint::kRenderPre, once_handler, nullptr, 1, 0) != 0);
    g_frame = 1;
    scheduler_dispatch(FramePoint::kRenderPre);
    CHECK(g_calls == 1);
    CHECK(scheduler_task_count(FramePoint::kRenderPre) == 0);

    g_frame = 2;
    scheduler_dispatch(FramePoint::kRenderPre);
    CHECK(g_calls == 1);
}

void test_dispatch_order_is_registration_order() {
    reset_all(-1);
    g_order[0] = '\0';
    g_order_n = 0;
    CHECK(scheduler_add(FramePoint::kLogicPre, order_a, nullptr, 1, 0) != 0);
    const FrameTaskId b = scheduler_add(FramePoint::kLogicPre, order_b, nullptr, 1, 0);
    CHECK(scheduler_add(FramePoint::kLogicPre, order_c, nullptr, 1, 0) != 0);

    g_frame = 1;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(std::strcmp(g_order, "ABC") == 0);

    // 删除中间任务后补一个：保序，新任务排在末尾。
    CHECK(scheduler_remove(b));
    CHECK(scheduler_add(FramePoint::kLogicPre, order_d, nullptr, 1, 0) != 0);
    g_order[0] = '\0';
    g_order_n = 0;
    g_frame = 2;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(std::strcmp(g_order, "ACD") == 0);
}

void test_point_isolation() {
    reset_all(-1);
    g_render_calls = 0;
    g_logic_calls = 0;
    CHECK(scheduler_add(FramePoint::kRenderPre, render_handler, nullptr, 1, 0) != 0);
    CHECK(scheduler_add(FramePoint::kLogicPre, logic_handler, nullptr, 1, 0) != 0);
    CHECK(scheduler_task_count(FramePoint::kRenderPre) == 1);
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 1);

    g_frame = 1;
    scheduler_dispatch(FramePoint::kRenderPre);
    CHECK(g_render_calls == 1);
    CHECK(g_logic_calls == 0);

    g_frame = 2;
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_logic_calls == 1);
    CHECK(g_render_calls == 1);

    scheduler_dispatch(static_cast<FramePoint>(9));  // 越界点位：无副作用
    CHECK(g_render_calls == 1);
    CHECK(g_logic_calls == 1);
}

void test_reset_for_test() {
    reset_all(1);
    CHECK(scheduler_add(FramePoint::kRenderPre, counting_handler, nullptr, 1, 0) != 0);
    CHECK(scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, 1, 0) != 0);
    CHECK(scheduler_task_count(FramePoint::kRenderPre) == 1);
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 1);

    scheduler_reset_for_test();
    CHECK(scheduler_task_count(FramePoint::kRenderPre) == 0);
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 0);
}

void test_no_clock_injected_skips_dispatch() {
    scheduler_reset_for_test();
    qol::infra::runtime_init(qol::infra::Clock{nullptr});  // 未注入
    CHECK(qol::infra::frame_count() == -1);
    g_calls = 0;
    CHECK(scheduler_add(FramePoint::kLogicPre, counting_handler, nullptr, 1, 0) != 0);
    scheduler_dispatch(FramePoint::kLogicPre);
    CHECK(g_calls == 0);
    CHECK(scheduler_task_count(FramePoint::kLogicPre) == 1);  // 任务保留，待时钟就绪
}

}  // namespace

int main() {
    test_add_remove_and_invalid();
    test_capacity_128();
    test_interval_and_frame_dedupe();
    test_count_decrement_and_auto_remove();
    test_handler_false_auto_remove();
    test_dispatch_order_is_registration_order();
    test_point_isolation();
    test_reset_for_test();
    test_no_clock_injected_skips_dispatch();
    std::printf("infra_scheduler: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
