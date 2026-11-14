// host 单测：统一日志系统 P0 纯格式化与限流原语。
// 编译 infra/log/qol_log.cpp，Android 专属头由 stubs/ 覆盖；不依赖设备与 I/O。
#include "infra/log/qol_log.h"
#include "infra/log/qol_crash.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <signal.h>
#include <unistd.h>

// 目录 → 域推导必须是编译期行为；任一映射写错或漏配在此直接编译失败。
static_assert(qol_domain_for_file("/w/module/app/src/main/cpp/plugins/core.save/x.cpp") ==
              QolDomain::kSave);
static_assert(qol_domain_for_file("/w/module/app/src/main/cpp/plugins/core.item/x.cpp") ==
              QolDomain::kInventory);
static_assert(qol_domain_for_file("/w/module/app/src/main/cpp/plugins/extension_bag/ui/x.cpp") ==
              QolDomain::kExtensionBag);
static_assert(qol_domain_for_file("/w/module/app/src/main/cpp/plugins/craft_lottery/f.cpp") ==
              QolDomain::kCraft);
static_assert(qol_domain_for_file("/w/module/app/src/main/cpp/plugins/core.custom_ui/p.cpp") ==
              QolDomain::kUi);
static_assert(qol_domain_for_file("/w/module/app/src/main/cpp/host/assembly.cpp") ==
              QolDomain::kCore);
static_assert(qol_domain_for_file("/w/module/app/src/main/cpp/unknown/x.cpp") ==
              QolDomain::kPlatform);

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

int64_t g_test_frame = 0;
int64_t g_test_now = 100000;

int64_t test_frame_provider() { return g_test_frame; }
int64_t test_clock() { return g_test_now; }

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
        QolDomain::kAutosell,   QolDomain::kCraft,    QolDomain::kAttrRange,
        QolDomain::kUi,         QolDomain::kConfig,      QolDomain::kCatalog,
        QolDomain::kCustomRecipe, QolDomain::kWorldTuning,
        QolDomain::kSpecialEquip,
    };
    const char* tokens[] = {
        "platform", "core",  "http",   "api",        "op",
        "inventory", "extension_bag", "save", "save_backup", "autosell",
        "craft", "attr_range", "ui", "config", "catalog",
        "custom_recipe", "world_tuning", "special_equip",
    };
    const size_t count = sizeof(domains) / sizeof(domains[0]);
    CHECK(count == static_cast<size_t>(QolDomain::kDomainCount));
    CHECK(count == 18);
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

// 落盘 src 恒为 basename:line：绝对构建路径既不泄漏也不允许被转发方缓冲截成半截名。
static void test_normalize_src() {
    char out[64];
    qol_log_normalize_src(out, sizeof(out), "/home/dev/proj/module/app/src/x.cpp:42");
    CHECK(std::strcmp(out, "x.cpp:42") == 0);
    qol_log_normalize_src(out, sizeof(out), "C:\\proj\\x.cpp:7");
    CHECK(std::strcmp(out, "x.cpp:7") == 0);
    qol_log_normalize_src(out, sizeof(out), "extension_bag");
    CHECK(std::strcmp(out, "extension_bag") == 0);
    qol_log_normalize_src(out, sizeof(out), nullptr);
    CHECK(out[0] == '\0');

    char tiny[5];
    const int required = qol_log_normalize_src(tiny, sizeof(tiny), "/a/abcdefgh");
    CHECK(required == 8);                          // 完整所需长度
    CHECK(std::strcmp(tiny, "abcd") == 0);         // cap-1 截断且 NUL 终止
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

namespace {

std::vector<std::string> g_sink_lines;

void collect_sink(QolLogLevel, QolDomain, const char* line) {
    g_sink_lines.emplace_back(line != nullptr ? line : "");
}

}  // namespace

static void test_logger_dedup() {
    qol_log_set_host_sink(&collect_sink);
    qol_log_set_test_clock(&test_clock);
    qol_log_set_frame_provider(&test_frame_provider);
    qol_log_set_debug_enabled(true);
    g_test_frame = 500; g_test_now = 100000;
    g_sink_lines.clear();
    for (int i = 0; i < 40; ++i) {
        qol_log_write_kotlin(QolLogLevel::kInfo, QolDomain::kUi, "x.kt:1", "tick");
    }
    CHECK(g_sink_lines.size() == 40);
    for (const auto& line : g_sink_lines) CHECK(line.find("cap reached") == std::string::npos);

    g_sink_lines.clear(); g_test_frame = 510;
    for (int i = 0; i < 3; ++i) qol_log_write_kotlin(QolLogLevel::kDebug, QolDomain::kCore, "d.kt:1", "a");
    CHECK(g_sink_lines.size() == 1);
    g_test_frame = 511; g_test_now += kQolLogDebugMinIntervalMs;
    qol_log_write_kotlin(QolLogLevel::kDebug, QolDomain::kCore, "d.kt:1", "b");
    CHECK(g_sink_lines.size() == 2);
    qol_log_write_kotlin(QolLogLevel::kDebug, QolDomain::kCore, "d.kt:1", "c");
    CHECK(g_sink_lines.size() == 2);
    g_test_frame = 512; g_test_now += kQolLogDebugMinIntervalMs;
    qol_log_write_kotlin(QolLogLevel::kDebug, QolDomain::kCore, "d.kt:1", "c");
    CHECK(g_sink_lines.size() == 3);
    qol_log_write_kotlin(QolLogLevel::kDebug, QolDomain::kCore, "d.kt:1", "d");
    CHECK(g_sink_lines.size() == 3);

    g_sink_lines.clear(); g_test_frame = 520;
    for (int i = 0; i < 3; ++i) qol_log_write_kotlin(QolLogLevel::kWarn, QolDomain::kCore, "w.kt:1", "same");
    CHECK(g_sink_lines.size() == 1);
    g_test_now += kQolLogWarnWindowMs;
    qol_log_write_kotlin(QolLogLevel::kWarn, QolDomain::kCore, "w.kt:1", "same");
    qol_log_write_kotlin(QolLogLevel::kWarn, QolDomain::kCore, "w.kt:1", "different");
    CHECK(g_sink_lines.size() == 3);

    g_sink_lines.clear();
    qol_log_write_kotlin(QolLogLevel::kError, QolDomain::kCore, "e.kt:1", "same");
    qol_log_write_kotlin(QolLogLevel::kError, QolDomain::kCore, "e.kt:1", "same");
    CHECK(g_sink_lines.size() == 1);
    g_test_now += kQolLogErrorWindowMs;
    qol_log_write_kotlin(QolLogLevel::kError, QolDomain::kCore, "e.kt:1", "same");
    CHECK(g_sink_lines.size() == 2);
    g_sink_lines.clear(); g_test_now += kQolLogErrorBurstWindowMs;
    for (int i = 0; i < 12; ++i) {
        char msg[24]; std::snprintf(msg, sizeof(msg), "failure=%d", i);
        qol_log_write_kotlin(QolLogLevel::kError, QolDomain::kCore, "burst.kt:1", msg);
    }
    CHECK(g_sink_lines.size() == 11);
    CHECK(g_sink_lines.back().find("suppressed level=E domain=core src=burst.kt:1 count=11 window_ms=60000") != std::string::npos);

    // SDK 转发的绝对路径落盘为 basename。
    g_sink_lines.clear();
    qol_log_write_kotlin(QolLogLevel::kInfo, QolDomain::kCore, "/proj/a/b.cpp:9", "m");
    CHECK(g_sink_lines.size() == 1);
    CHECK(g_sink_lines[0].find(" b.cpp:9 m") != std::string::npos);
    CHECK(g_sink_lines[0].find("/proj/") == std::string::npos);

    g_sink_lines.clear();
    for (int i = 0; i < 600; ++i) {
        char src[32]; std::snprintf(src, sizeof(src), "overflow%d.kt:1", i);
        qol_log_write_kotlin(QolLogLevel::kWarn, QolDomain::kCore, src, "x");
    }
    CHECK(g_sink_lines.size() == 600);

    qol_log_set_host_sink(nullptr);
    qol_log_set_test_clock(nullptr);
    qol_log_set_debug_enabled(false);
    qol_log_set_frame_provider(nullptr);
}

// 崩溃处理器读的是缓存帧号：不能回调游戏帧源（崩溃时可能持有未释放的锁）。
static void test_last_known_frame() {
    qol_log_set_host_sink(&collect_sink);
    qol_log_set_frame_provider(&test_frame_provider);
    g_sink_lines.clear();

    g_test_frame = 777;
    qol_log_write_kotlin(QolLogLevel::kInfo, QolDomain::kSave, "x.kt:1", "tick");
    CHECK(qol_log_last_known_frame() == 777);

    qol_log_set_frame_provider(nullptr);
    CHECK(qol_log_last_known_frame() == 777);  // 帧源摘除后保留最后已知值

    qol_log_set_host_sink(nullptr);
}

// 崩溃现场：信号/寄存器/maps/日志尾部四段都要落盘，且 agent 能离线符号化。
static void test_crash_capture() {
    char dir_template[] = "./qol_crash_test_XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    CHECK(dir != nullptr);
    if (dir == nullptr) return;

    const std::string log_path = std::string(dir) + "/qol.log";
    std::FILE* log_file = std::fopen(log_path.c_str(), "w");
    CHECK(log_file != nullptr);
    if (log_file != nullptr) {
        std::fputs("last-line-before-crash\n", log_file);
        std::fclose(log_file);
    }

    qol_crash_init(dir, log_path.c_str());
    CHECK(qol_crash_simulate(SIGSEGV, SEGV_MAPERR, 0x1234));

    const std::string crash_path = std::string(dir) + "/qol-crash.log";
    std::string content;
    std::FILE* crash_file = std::fopen(crash_path.c_str(), "r");
    CHECK(crash_file != nullptr);
    if (crash_file != nullptr) {
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), crash_file)) > 0) content.append(buf, n);
        std::fclose(crash_file);
    }

    CHECK(content.find("=== qol crash ===") != std::string::npos);
    CHECK(content.find("signal=11 SIGSEGV code=1 (SEGV_MAPERR) fault=0x1234") !=
          std::string::npos);
    CHECK(content.find("pid=") != std::string::npos);
    CHECK(content.find("frame=") != std::string::npos);
    CHECK(content.find("--- maps ---") != std::string::npos);
    CHECK(content.find("qol_log_tests") != std::string::npos);  // 可执行文件必须在 maps 里
#if !defined(__aarch64__)
    // 回溯只在 arm64/Linux 上实现；其余平台写固定标记，符号化脚本按 key=value 忽略。
    CHECK(content.find("backtrace=unavailable") != std::string::npos);
#endif
    CHECK(content.find("--- log tail ---") != std::string::npos);
    CHECK(content.find("last-line-before-crash") != std::string::npos);

    std::remove(crash_path.c_str());
    std::remove(log_path.c_str());
    ::rmdir(dir);
}

int main() {
    test_format_info_exact_line();
    test_level_chars();
    test_escape_rules();
    test_frame_missing();
    test_domain_tokens();
    test_basename();
    test_normalize_src();
    test_format_ts_shape();
    test_buffer_truncation();
    test_kotlin_write_debug_gate();
    test_debug_flag_and_sink_host();
    test_logger_dedup();
    test_last_known_frame();
    test_crash_capture();

    std::printf("qol_log_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
