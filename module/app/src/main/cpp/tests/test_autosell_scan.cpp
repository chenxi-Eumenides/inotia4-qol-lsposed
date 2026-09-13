#include "feature/autosell/autosell_scan.h"

#include <cstdint>
#include <cstdio>

// host 单测（M-2）：autosell 60 帧节流纯函数 autosell_should_scan。
// 无游戏/Android 依赖，仅包含声明该 inline 函数的头文件。

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_immediate_always_scans() {
    // 立即执行请求无条件跳过 60 帧节流，含帧号无效/回退。
    CHECK(autosell_should_scan(1, -1, true));
    CHECK(autosell_should_scan(1000, 995, true));   // 仅差 5 帧
    CHECK(autosell_should_scan(0, 0, true));        // 帧号无效也扫描
    CHECK(autosell_should_scan(-5, 100, true));     // 回退也扫描
}

static void test_invalid_frame() {
    // 非立即且帧号无效（<=0）不扫描。
    CHECK(!autosell_should_scan(0, -1, false));
    CHECK(!autosell_should_scan(-3, 10, false));
    CHECK(!autosell_should_scan(0, 0, false));
}

static void test_never_scanned() {
    // last<0（从未扫描）且帧号有效：首次即扫描，不受 60 帧间隔限制。
    CHECK(autosell_should_scan(1, -1, false));
    CHECK(autosell_should_scan(5, -1, false));
    CHECK(autosell_should_scan(42, -1, false));
}

static void test_frame_rollback() {
    // 帧号回退（异常/重启）视为应扫描，不静默丢弃。
    CHECK(autosell_should_scan(2, 3, false));
    CHECK(autosell_should_scan(100, 1000, false));
}

static void test_interval_boundary() {
    // 正常前进：距上次 <60 帧不扫描，>=60 帧扫描。
    CHECK(!autosell_should_scan(1059, 1000, false));  // 差 59
    CHECK(autosell_should_scan(1060, 1000, false));   // 差 60
    CHECK(autosell_should_scan(1100, 1000, false));   // 差 100
    CHECK(!autosell_should_scan(1001, 1000, false));  // 差 1
    CHECK(!autosell_should_scan(1000, 1000, false));  // 差 0
}

int main() {
    test_immediate_always_scans();
    test_invalid_frame();
    test_never_scanned();
    test_frame_rollback();
    test_interval_boundary();
    std::printf("autosell_scan_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
