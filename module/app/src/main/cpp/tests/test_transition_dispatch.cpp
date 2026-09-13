// host 单测：状态转换派发纯逻辑 + 逻辑帧点枚举。
// 只包含头文件纯逻辑（transition_detail / FramePointId），不链接运行期派发器。
#include "core/native/frame_task.h"
#include "core/native/transition_dispatch.h"

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_frame_points() {
    // 多点位设计：新增逻辑帧点后，点位数量与编号必须稳定。
    CHECK(kFramePointRenderPre == 0);
    CHECK(kFramePointLogicPre == 1);
    CHECK(kFramePointCount == 2);
}

static void test_transition_detail() {
    // 单飞门：先前无在途 -> 放行；已有在途 -> busy（防止调用点双重否定写反）。
    CHECK(transition_detail::submit_decision(false) ==
          transition_detail::SubmitDecision::kProceed);
    CHECK(transition_detail::submit_decision(true) ==
          transition_detail::SubmitDecision::kBusy);

    // 超时：未消费可取消；已消费必须等待完成。
    CHECK(transition_detail::on_timeout(false) == transition_detail::TimeoutAction::kCancelPending);
    CHECK(transition_detail::on_timeout(true) == transition_detail::TimeoutAction::kWaitForCompletion);
}

int main() {
    test_frame_points();
    test_transition_detail();
    std::printf("transition_dispatch_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
