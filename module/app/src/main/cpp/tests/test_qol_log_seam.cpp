// T2l-w-write：game/access/qol_log_seam.h 断言（W36/W37）。
// 覆盖：未注入静默降级、绑定后输出序数（level=0/kApi=3）与 src/正文、debug 开关门控。
// 节流与去重在 native 统一 logger 内部，seam 不再维护帧号状态。

#include "contracts/host_sdk.h"
#include "game/access/qol_log_seam.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int64_t g_test_frame = 0;
int64_t qol::game::frame_count() { return g_test_frame; }

namespace {

int32_t g_debug_enabled = 1;
int32_t capture_level = -1;
int32_t capture_domain = -1;
std::string capture_source;
std::string capture_message;

QolResult fake_log_write(int32_t level, int32_t domain, const char* source, const char* message,
                         uint32_t message_len) {
    capture_level = level;
    capture_domain = domain;
    capture_source = source != nullptr ? source : "";
    capture_message = message != nullptr ? std::string(message, message_len) : std::string();
    return qol::contracts::ok();
}

int32_t fake_debug_enabled() { return g_debug_enabled; }

qol::contracts::QolHostSdk g_fake_sdk{};

}  // namespace

int main() {
    using qol::game::qol_seam_bind_host_sdk;
    using qol::game::qol_seam_debug_enabled;
    using qol::game::qol_seam_write;

    // W37：未注入 → 静默降级（不输出、不崩溃）。
    g_test_frame = -1;
    CHECK(!qol_seam_debug_enabled());
    qol_seam_write(0, 3, __FILE__, __LINE__, "noop %d", 1);
    CHECK(capture_level == -1);

    // W36：绑定后序数与正文。
    g_fake_sdk.log.write = &fake_log_write;
    g_fake_sdk.log.debug_enabled = &fake_debug_enabled;
    qol_seam_bind_host_sdk(&g_fake_sdk);
    CHECK(qol_seam_debug_enabled());
    qol_seam_write(0, 3, "/gamesrc/world.cpp", 42, "replan: wait-retry ok dirs=%d", 3);
    CHECK(capture_level == 0);    // QolLogLevel::kDebug
    CHECK(capture_domain == 3);   // QolDomain::kApi
    CHECK(capture_source == "world.cpp:42");
    CHECK(capture_message == "replan: wait-retry ok dirs=3");

    // debug 关闭 → 不输出。
    g_debug_enabled = 0;
    CHECK(!qol_seam_debug_enabled());
    capture_level = -1;
    qol_seam_write(0, 3, __FILE__, __LINE__, "hidden %d", 1);
    CHECK(capture_level == -1);
    g_debug_enabled = 1;

    std::printf("qol_log_seam: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
