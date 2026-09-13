// host 单测：统一日志系统 P0 纯格式化与限流原语。
// 编译 core/native/qol_log.cpp，Android 专属头由 stubs/ 覆盖；不依赖设备与 I/O。
#include "core/native/qol_log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

int64_t g_test_frame = 0;

int64_t test_frame_provider() { return g_test_frame; }

int g_frame_provider_calls = 0;

int64_t counting_frame_provider() {
    ++g_frame_provider_calls;
    return 42;
}

}  // namespace

static void test_format_info_exact_line() {
    char out[256];
    const char* ts = "2026-09-14 12:34:56.789";
    const int n = qol_log_format(out, sizeof(out), ts, 120483, QolLogLevel::kInfo,
                                 QolDomain::kOp, "OpApiService.kt:42",
                                 "endpoint=GET /api/x");
    CHECK(std::strcmp(out,
                      "2026-09-14 12:34:56.789 f=120483 I op OpApiService.kt:42 "
                      "endpoint=GET /api/x") == 0);
    CHECK(n == static_cast<int>(std::strlen(out)));
}

static void test_level_chars() {
    char out[128];
    qol_log_format(out, sizeof(out), "T", 1, QolLogLevel::kWarn, QolDomain::kCore,
                   "a.cpp:1", "m");
    CHECK(std::strcmp(out, "T f=1 W core a.cpp:1 m") == 0);

    qol_log_format(out, sizeof(out), "T", 1, QolLogLevel::kError, QolDomain::kCore,
                   "a.cpp:1", "m");
    CHECK(std::strcmp(out, "T f=1 E core a.cpp:1 m") == 0);

    qol_log_format(out, sizeof(out), "T", 1, QolLogLevel::kDebug, QolDomain::kCore,
                   "a.cpp:1", "m");
    CHECK(std::strcmp(out, "T f=1 D core a.cpp:1 m") == 0);
}

static void test_escape_rules() {
    char out[128];
    // 非 debug：'\n' 转义为字面 "\n"，'\r' 丢弃。
    qol_log_format(out, sizeof(out), "T", 1, QolLogLevel::kInfo, QolDomain::kCore,
                   "a.cpp:1", "a\nb");
    CHECK(std::strcmp(out, "T f=1 I core a.cpp:1 a\\nb") == 0);

    qol_log_format(out, sizeof(out), "T", 1, QolLogLevel::kWarn, QolDomain::kCore,
                   "a.cpp:1", "x\ry");
    CHECK(std::strcmp(out, "T f=1 W core a.cpp:1 xy") == 0);

    // debug：保留真实换行。
    qol_log_format(out, sizeof(out), "T", 1, QolLogLevel::kDebug, QolDomain::kCore,
                   "a.cpp:1", "a\nb");
    CHECK(std::strcmp(out, "T f=1 D core a.cpp:1 a\nb") == 0);
}

static void test_frame_missing() {
    qol_log_set_frame_provider(nullptr);
    CHECK(qol_log_current_frame() == -1);

    char out[128];
    qol_log_format(out, sizeof(out), "T", qol_log_current_frame(), QolLogLevel::kInfo,
                   QolDomain::kCore, "a.cpp:1", "m");
    CHECK(std::strcmp(out, "T f=-1 I core a.cpp:1 m") == 0);
}

static void test_domain_tokens() {
    const QolDomain domains[] = {
        QolDomain::kPlatform,   QolDomain::kCore,        QolDomain::kHttp,
        QolDomain::kApi,        QolDomain::kOp,          QolDomain::kInventory,
        QolDomain::kExtensionBag, QolDomain::kSave,      QolDomain::kSaveBackup,
        QolDomain::kAutosell,   QolDomain::kGemCraft,    QolDomain::kAttrRange,
        QolDomain::kUi,         QolDomain::kConfig,      QolDomain::kCatalog,
    };
    const char* tokens[] = {
        "platform", "core",  "http",   "api",        "op",
        "inventory", "extension_bag", "save", "save_backup", "autosell",
        "gem_craft", "attr_range", "ui", "config", "catalog",
    };
    const size_t count = sizeof(domains) / sizeof(domains[0]);
    CHECK(count == 15);
    for (size_t i = 0; i < count; ++i) {
        CHECK(std::strcmp(qol_domain_token(domains[i]), tokens[i]) == 0);
        CHECK(qol_domain_from_token(tokens[i]) == domains[i]);
    }
    CHECK(qol_domain_from_token("nonsense") == QolDomain::kPlatform);
    CHECK(qol_domain_from_token(nullptr) == QolDomain::kPlatform);
}

static void test_basename() {
    CHECK(std::strcmp(qol_log_basename("/a/b/c.cpp"), "c.cpp") == 0);
    CHECK(std::strcmp(qol_log_basename("c.cpp"), "c.cpp") == 0);
    CHECK(std::strcmp(qol_log_basename("C:\\a\\b.cpp"), "b.cpp") == 0);
}

static void test_format_ts_shape() {
    char ts[32];
    qol_log_format_ts(ts, sizeof(ts), 0);
    CHECK(std::strlen(ts) == 23);
    CHECK(ts[4] == '-');
    CHECK(ts[7] == '-');
    CHECK(ts[10] == ' ');
    CHECK(ts[13] == ':');
    CHECK(ts[16] == ':');
    CHECK(ts[19] == '.');
}

static void test_buffer_truncation() {
    char small[8];
    const int required = qol_log_format(small, sizeof(small), "T", 1, QolLogLevel::kInfo,
                                        QolDomain::kCore, "a", "b");
    CHECK(required == static_cast<int>(std::strlen("T f=1 I core a b")));
    CHECK(std::strlen(small) == sizeof(small) - 1);
}

static void test_throttle_frames() {
    qol_log_set_frame_provider(&test_frame_provider);

    g_test_frame = 10;
    QolLogThrottle throttle;
    CHECK(throttle.due(5));       // 首次放行
    CHECK(!throttle.due(5));      // 同帧不放行
    g_test_frame = 14;
    CHECK(!throttle.due(5));      // 差 4 < 5
    g_test_frame = 15;
    CHECK(throttle.due(5));       // 差 5 >= 5
    g_test_frame = 19;
    CHECK(!throttle.due(5));
    g_test_frame = 20;
    CHECK(throttle.due(5));

    // 无 provider -> 时间窗口（首次放行，连续第二次不放行）。
    qol_log_set_frame_provider(nullptr);
    QolLogThrottle fallback;
    CHECK(fallback.due(5));
    CHECK(!fallback.due(5));
}

static void test_change_gate() {
    QolLogChangeGate gate;
    CHECK(gate.changed(7));    // 首次：与初值不同 -> 触发
    CHECK(!gate.changed(7));   // 同值不触发
    CHECK(gate.changed(8));    // 变值触发
    CHECK(!gate.changed(8));
    CHECK(gate.changed(0));
}

// 修复 1：qol_log_write_kotlin 对 kDebug 走 qol_log_debug_enabled() 门控。
// 通过帧 provider 调用次数间接观测是否进入 emit_line（格式化前短路则 provider 不被调用）。
static void test_kotlin_write_debug_gate() {
    qol_log_set_frame_provider(&counting_frame_provider);

    qol_log_set_debug_enabled(false);
    g_frame_provider_calls = 0;
    qol_log_write_kotlin(QolLogLevel::kDebug, QolDomain::kCore, "a.kt:1", "d");
    CHECK(g_frame_provider_calls == 0);  // debug 关闭：格式化前短路，零开销

    qol_log_set_debug_enabled(true);
    g_frame_provider_calls = 0;
    qol_log_write_kotlin(QolLogLevel::kDebug, QolDomain::kCore, "a.kt:1", "d");
    CHECK(g_frame_provider_calls == 1);  // debug 开启：正常发射

    // 非 debug 级别不受开关影响。
    qol_log_set_debug_enabled(false);
    g_frame_provider_calls = 0;
    qol_log_write_kotlin(QolLogLevel::kInfo, QolDomain::kCore, "a.kt:1", "i");
    CHECK(g_frame_provider_calls == 1);
    g_frame_provider_calls = 0;
    qol_log_write_kotlin(QolLogLevel::kWarn, QolDomain::kCore, "a.kt:1", "w");
    CHECK(g_frame_provider_calls == 1);
    g_frame_provider_calls = 0;
    qol_log_write_kotlin(QolLogLevel::kError, QolDomain::kCore, "a.kt:1", "e");
    CHECK(g_frame_provider_calls == 1);

    // 空指针输入不崩溃。
    qol_log_write_kotlin(QolLogLevel::kDebug, QolDomain::kCore, nullptr, nullptr);
    qol_log_write_kotlin(QolLogLevel::kInfo, QolDomain::kCore, nullptr, nullptr);

    qol_log_set_frame_provider(nullptr);
    qol_log_set_debug_enabled(false);
}

// 修复 2：帧号回退后立即放行；有效帧 → 无效帧重置基准，恢复后按首次放行。
static void test_throttle_frame_rollback() {
    qol_log_set_frame_provider(&test_frame_provider);

    g_test_frame = 1000;
    QolLogThrottle throttle;
    CHECK(throttle.due(5));   // 首次放行
    g_test_frame = 1005;
    CHECK(throttle.due(5));   // 正常间隔放行
    g_test_frame = 3;         // 计数器回退/重开档（frame < last_frame）
    CHECK(throttle.due(5));   // 立即放行，不等长间隔
    g_test_frame = 4;
    CHECK(!throttle.due(5));  // 回退后从新基准 3 起算
    g_test_frame = 7;
    CHECK(!throttle.due(5));  // 差 4 < 5
    g_test_frame = 8;
    CHECK(throttle.due(5));   // 差 5 >= 5

    // 有效帧 → 帧号无效：重置基准并放行一次，之后退化为时间窗口。
    g_test_frame = 50;
    QolLogThrottle nested;
    CHECK(nested.due(10));    // 首次放行
    g_test_frame = -1;
    CHECK(nested.due(10));    // 有效→无效：重置并放行
    CHECK(!nested.due(10));   // 连续无效：时间窗口内不放行
    g_test_frame = 2;
    CHECK(nested.due(10));    // 帧恢复：按首次调用放行

    qol_log_set_frame_provider(nullptr);
}

static void test_debug_flag_and_sink_host() {
    CHECK(!qol_log_debug_enabled());
    qol_log_set_debug_enabled(true);
    CHECK(qol_log_debug_enabled());
    qol_log_set_debug_enabled(false);
    CHECK(!qol_log_debug_enabled());

    qol_log_init();                 // host：空操作，幂等
    qol_log_init();
    CHECK(!qol_log_file_ready());   // host 无文件 sink
}

int main() {
    test_format_info_exact_line();
    test_level_chars();
    test_escape_rules();
    test_frame_missing();
    test_domain_tokens();
    test_basename();
    test_format_ts_shape();
    test_buffer_truncation();
    test_throttle_frames();
    test_change_gate();
    test_kotlin_write_debug_gate();
    test_throttle_frame_rollback();
    test_debug_flag_and_sink_host();

    std::printf("qol_log_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
