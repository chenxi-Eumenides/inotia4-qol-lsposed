// qol_log.cpp —— 统一日志系统 P0 基础设施（native 侧）实现。
//
// 职责：纯格式化（host 可测）+ 发射（logcat + 单写者文件 sink）+ 限流原语。
// 依赖：仅 core（STL + <android/log.h>）；不依赖 feature/bridge。
// host 构建：跳过文件 sink 与 /proc/self/cmdline，logcat 走 stubs 空操作桩。

#include "infra/log/qol_log.h"

#include "infra/log/qol_crash.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr char kTag[] = "Inotia4Qol";
constexpr char kDefaultPackage[] =
    "com.com2us.inotia4.normal.freefull.google.global.android.common";
constexpr size_t kMsgCap = 4096;
constexpr size_t kDedupCapacity = 512;
constexpr size_t kDedupProbeLimit = 16;

struct DomainEntry {
    QolDomain domain;
    const char* token;
};

// 与 QolDomain 枚举逐项对应；顺序不重要，token 文本是唯一契约。
const DomainEntry kDomainTable[] = {
    {QolDomain::kPlatform, "platform"},
    {QolDomain::kCore, "core"},
    {QolDomain::kHttp, "http"},
    {QolDomain::kApi, "api"},
    {QolDomain::kOp, "op"},
    {QolDomain::kInventory, "inventory"},
    {QolDomain::kExtensionBag, "extension_bag"},
    {QolDomain::kSave, "save"},
    {QolDomain::kSaveBackup, "save_backup"},
    {QolDomain::kAutosell, "autosell"},
    {QolDomain::kCraft, "craft"},
    {QolDomain::kAttrRange, "attr_range"},
    {QolDomain::kUi, "ui"},
    {QolDomain::kConfig, "config"},
    {QolDomain::kCatalog, "catalog"},
    {QolDomain::kCustomRecipe, "custom_recipe"},
    {QolDomain::kWorldTuning, "world_tuning"},
    {QolDomain::kSpecialEquip, "special_equip"},
    {QolDomain::kItemLab, "item_lab"},
};

std::atomic<QolFrameProvider> g_frame_provider{nullptr};
std::atomic<int64_t> g_last_known_frame{-1};
std::atomic<bool> g_debug_enabled{false};
std::atomic<int> g_sink_fd{-1};
#if !defined(__ANDROID__)
std::atomic<QolLogHostSink> g_host_sink{nullptr};
#endif
std::mutex g_sink_mtx;
std::once_flag g_sink_once;
std::string g_sink_path;                 // 受 g_sink_mtx 保护
int64_t g_sink_bytes = 0;                // 受 g_sink_mtx 保护
thread_local bool g_in_emit = false;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

char level_char(QolLogLevel level) {
    switch (level) {
        case QolLogLevel::kDebug: return 'D';
        case QolLogLevel::kInfo:  return 'I';
        case QolLogLevel::kWarn:  return 'W';
        case QolLogLevel::kError: return 'E';
    }
    return 'I';
}

int level_priority(QolLogLevel level) {
    switch (level) {
        case QolLogLevel::kDebug: return ANDROID_LOG_DEBUG;
        case QolLogLevel::kInfo:  return ANDROID_LOG_INFO;
        case QolLogLevel::kWarn:  return ANDROID_LOG_WARN;
        case QolLogLevel::kError: return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_INFO;
}

// 只写不抛的整行构造器：len_ 始终记录“完整所需长度”，载体不足则截断并保证 NUL。
class LineBuilder {
public:
    LineBuilder(char* out, size_t cap) : out_(out), cap_(cap) {
        if (cap_ > 0) out_[0] = '\0';
    }

    void put(char c) {
        if (len_ + 1 < cap_) out_[len_] = c;
        ++len_;
    }

    void put_str(const char* s) {
        if (s == nullptr) return;
        while (*s != '\0') put(*s++);
    }

    void put_i64(int64_t value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        put_str(buf);
    }

    // escape=true：'\n' → 字面 "\\n"，丢弃 '\r'；escape=false：保留原样（仅 debug）。
    void put_msg(const char* msg, bool escape) {
        if (msg == nullptr) return;
        for (const char* p = msg; *p != '\0'; ++p) {
            if (*p == '\r') {
                if (!escape) put(*p);
            } else if (*p == '\n') {
                if (escape) {
                    put('\\');
                    put('n');
                } else {
                    put(*p);
                }
            } else {
                put(*p);
            }
        }
    }

    int finish() {
        if (cap_ > 0) {
            const size_t written = len_ < cap_ ? len_ : cap_ - 1;
            out_[written] = '\0';
        }
        return static_cast<int>(len_);
    }

private:
    char* out_;
    size_t cap_;
    size_t len_ = 0;
};

struct DedupEntry {
    uint64_t key_hash = 0;
    uint64_t msg_hash = 0;
    int64_t last_ms = 0;
    int64_t last_frame = -1;
    int64_t window_start_ms = 0;
    uint32_t window_count = 0;
    bool occupied = false;
};
DedupEntry g_dedup[kDedupCapacity];
std::mutex g_dedup_mtx;
#if !defined(__ANDROID__)
std::atomic<QolLogTestClock> g_test_clock{nullptr};
#endif

uint64_t hash_bytes(uint64_t hash, const char* value) {
    if (value != nullptr) for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t message_hash(const char* msg) { return hash_bytes(14695981039346656037ULL, msg); }
uint64_t source_hash(const char* src, QolLogLevel level) {
    uint64_t hash = hash_bytes(14695981039346656037ULL, src);
    hash ^= static_cast<uint8_t>(level);
    return hash * 1099511628211ULL;
}

int64_t log_now_ms() {
#if !defined(__ANDROID__)
    const QolLogTestClock clock = g_test_clock.load(std::memory_order_acquire);
    if (clock != nullptr) return clock();
#endif
    return now_ms();
}

DedupEntry& find_entry(uint64_t key) {
    size_t oldest = 0;
    for (size_t probe = 0; probe < kDedupProbeLimit; ++probe) {
        const size_t index = (key + probe) % kDedupCapacity;
        DedupEntry& entry = g_dedup[index];
        if (entry.occupied && entry.key_hash == key) return entry;
        if (!entry.occupied) return entry;
        if (g_dedup[oldest].last_ms > entry.last_ms) oldest = index;
    }
    return g_dedup[oldest];
}

// 返回 true 表示抑制；summary 非空时调用方发射该汇总行。
bool dedup_suppress(QolLogLevel level, QolDomain domain, const char* src, const char* msg, int64_t now,
                    int64_t frame, char* summary, size_t summary_cap) {
    if (level == QolLogLevel::kInfo) return false;
    const uint64_t key = source_hash(src, level);
    const uint64_t body = message_hash(msg);
    std::lock_guard<std::mutex> lock(g_dedup_mtx);
    DedupEntry& entry = find_entry(key);
    const bool same = entry.occupied && entry.key_hash == key;
    if (level == QolLogLevel::kDebug && same && entry.msg_hash == body) return true;
    if (level == QolLogLevel::kDebug && same && frame >= 0 && frame == entry.last_frame) return true;
    if (level == QolLogLevel::kDebug && same && now - entry.last_ms < kQolLogDebugMinIntervalMs) return true;
    if (level == QolLogLevel::kWarn && same && entry.msg_hash == body && now - entry.last_ms < kQolLogWarnWindowMs) return true;
    if (level == QolLogLevel::kError && same && entry.msg_hash == body && now - entry.last_ms < kQolLogErrorWindowMs) return true;
    if (!same) {
        entry = DedupEntry{};
        entry.occupied = true;
        entry.key_hash = key;
        entry.window_start_ms = now;
    }
    if (level == QolLogLevel::kError) {
        if (now - entry.window_start_ms >= kQolLogErrorBurstWindowMs || now < entry.window_start_ms) {
            entry.window_start_ms = now;
            entry.window_count = 0;
        }
        if (entry.window_count >= kQolLogErrorBurstLimit) {
            if (entry.window_count == kQolLogErrorBurstLimit) {
                ++entry.window_count;
                std::snprintf(summary, summary_cap,
                              "suppressed level=E domain=%s src=%s count=%u window_ms=%lld",
                              qol_domain_token(domain), src, entry.window_count,
                              static_cast<long long>(kQolLogErrorBurstWindowMs));
                return false;
            }
            return true;
        }
        ++entry.window_count;
    }
    entry.msg_hash = body;
    entry.last_ms = now;
    entry.last_frame = frame;
    return false;
}

#if defined(__ANDROID__)
// 单文件写满后轮转为 `<path>.1`，只保留一份旧文件。调用方必须持有 g_sink_mtx。
void rotate_locked(int* out_fd) {
    const int fd = g_sink_fd.load(std::memory_order_acquire);
    if (fd >= 0) close(fd);
    const std::string rotated = g_sink_path + ".1";
    (void)::unlink(rotated.c_str());
    (void)::rename(g_sink_path.c_str(), rotated.c_str());
    int reopened = ::open(g_sink_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    g_sink_bytes = 0;
    g_sink_fd.store(reopened, std::memory_order_release);
    *out_fd = reopened;
    if (reopened < 0) return;
    char ts[32];
    qol_log_format_ts(ts, sizeof(ts), now_ms());
    char banner[96];
    const int banner_len = std::snprintf(banner, sizeof(banner),
                                         "=== Inotia4Qol log start %s ===\n", ts);
    if (banner_len > 0) {
        const ssize_t ignored = write(reopened, banner, static_cast<size_t>(banner_len));
        (void)ignored;
        g_sink_bytes += banner_len;
    }
}
#endif

void emit_line(QolLogLevel level, QolDomain domain, const char* src, const char* msg) {
    if (level == QolLogLevel::kDebug && !qol_log_debug_enabled()) return;
    if (g_in_emit) return;  // 递归门禁：格式化/日志后端重入直接返回
    g_in_emit = true;

    const int64_t frame = qol_log_current_frame();
    if (frame >= 0) g_last_known_frame.store(frame, std::memory_order_release);
    const int64_t now = log_now_ms();
    char summary[256] = {};
    if (dedup_suppress(level, domain, src, msg, now, frame, summary, sizeof(summary))) {
        g_in_emit = false;
        return;
    }
    const char* out_msg = summary[0] != '\0' ? summary : msg;

    char ts[32];
    qol_log_format_ts(ts, sizeof(ts), now);

    char line[5120];
    const int required = qol_log_format(line, sizeof(line), ts, frame,
                                        level, domain, src, out_msg);
    (void)required;  // host 无文件 sink 时不使用返回值

    __android_log_print(level_priority(level), kTag, "%s", line);
#if !defined(__ANDROID__)
    const QolLogHostSink sink = g_host_sink.load(std::memory_order_acquire);
    if (sink != nullptr) sink(level, domain, line);
#endif

#if defined(__ANDROID__)
    const size_t len = required > 0 ? static_cast<size_t>(required) : 0;
    const size_t stored = len < sizeof(line) ? len : sizeof(line) - 1;
    {
        std::lock_guard<std::mutex> lock(g_sink_mtx);
        int fd = g_sink_fd.load(std::memory_order_acquire);
        if (fd >= 0 && g_sink_bytes + static_cast<int64_t>(stored) + 1 > kQolLogFileCapBytes) {
            rotate_locked(&fd);
        }
        if (fd >= 0) {
            // 单写者：line + '\n' 在同一临界区内落盘。
            if (write(fd, line, stored) >= 0) {
                const char newline = '\n';
                const ssize_t ignored = write(fd, &newline, 1);
                (void)ignored;
                g_sink_bytes += static_cast<int64_t>(stored) + 1;
            }
        }
    }
#endif

    g_in_emit = false;
}

}  // namespace

const char* qol_domain_token(QolDomain domain) {
    for (const DomainEntry& entry : kDomainTable) {
        if (entry.domain == domain) return entry.token;
    }
    return kDomainTable[0].token;  // kPlatform
}

QolDomain qol_domain_from_token(const char* token) {
    if (token == nullptr) return QolDomain::kPlatform;
    for (const DomainEntry& entry : kDomainTable) {
        if (std::strcmp(entry.token, token) == 0) return entry.domain;
    }
    return QolDomain::kPlatform;
}

const char* qol_log_basename(const char* path) {
    if (path == nullptr) return "";
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

int qol_log_normalize_src(char* out, size_t cap, const char* src) {
    if (out == nullptr || cap == 0) return 0;
    const char* base = qol_log_basename(src);
    const size_t len = std::strlen(base);
    const size_t keep = len < cap ? len : cap - 1;
    std::memcpy(out, base, keep);
    out[keep] = '\0';
    return static_cast<int>(len);
}

void qol_log_set_frame_provider(QolFrameProvider fn) {
    g_frame_provider.store(fn, std::memory_order_release);
}

int64_t qol_log_current_frame() {
    const QolFrameProvider fn = g_frame_provider.load(std::memory_order_acquire);
    if (fn == nullptr) return -1;
    const int64_t frame = fn();
    return frame < 0 ? -1 : frame;
}

int64_t qol_log_last_known_frame() {
    return g_last_known_frame.load(std::memory_order_acquire);
}

bool qol_log_debug_enabled() {
    return g_debug_enabled.load(std::memory_order_acquire);
}

void qol_log_set_debug_enabled(bool enabled) {
    g_debug_enabled.store(enabled, std::memory_order_release);
}

#if !defined(__ANDROID__)
void qol_log_set_host_sink(QolLogHostSink sink) {
    g_host_sink.store(sink, std::memory_order_release);
}
void qol_log_set_test_clock(QolLogTestClock fn) {
    g_test_clock.store(fn, std::memory_order_release);
}
#endif

void qol_log_init() {
#if defined(__ANDROID__)
    std::call_once(g_sink_once, [] {
        char cmdline[256] = {};
        const int cfd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
        const ssize_t n = cfd >= 0 ? read(cfd, cmdline, sizeof(cmdline) - 1) : -1;
        if (cfd >= 0) close(cfd);
        const char* package = kDefaultPackage;
        if (n > 0) {
            cmdline[n] = '\0';
            package = cmdline;
        }
        const std::string log_dir =
            "/sdcard/Android/data/" + std::string(package) + "/files/qol";
        const bool directory_ready = ::mkdir(log_dir.c_str(), 0755) == 0 || errno == EEXIST;
        const std::string path = log_dir + "/qol.log";
        // 保留上一会话日志：先挪走轮转文件再挪走主文件，顺序固定（若先挪主文件，
        // 旧 .1 会在下次轮转被覆盖，失去 prev.1 语义）。失败（含 ENOENT）一律忽略，
        // 不得阻断日志初始化。
        (void)::rename((path + ".1").c_str(), (path + ".prev.1").c_str());
        (void)::rename(path.c_str(), (path + ".prev").c_str());
        const int fd = directory_ready
                           ? open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)
                           : -1;
        if (fd < 0) return;
        g_sink_path = path;
        g_sink_fd.store(fd, std::memory_order_release);

        char ts[32];
        qol_log_format_ts(ts, sizeof(ts), now_ms());
        char banner[96];
        const int banner_len = std::snprintf(banner, sizeof(banner),
                                             "=== Inotia4Qol log start %s ===\n", ts);
        if (banner_len > 0) {
            const ssize_t ignored = write(fd, banner, static_cast<size_t>(banner_len));
            (void)ignored;
            g_sink_bytes += banner_len;
        }
        // 崩溃捕获与主日志共用目录：崩溃文件里会复制 qol.log 尾部作为现场。
        qol_crash_init(log_dir.c_str(), path.c_str());
    });
#else
    std::call_once(g_sink_once, [] {});  // host：无文件 sink
#endif
}

bool qol_log_file_ready() {
#if defined(__ANDROID__)
    return g_sink_fd.load(std::memory_order_acquire) >= 0;
#else
    return false;
#endif
}

void qol_log_format_ts(char* out, size_t cap, int64_t ts_ms) {
    if (out == nullptr || cap == 0) return;
    const std::time_t seconds = static_cast<std::time_t>(ts_ms / 1000);
    int millis = static_cast<int>(ts_ms % 1000);
    if (millis < 0) millis += 1000;
    std::tm local_time{};
    localtime_r(&seconds, &local_time);
    std::snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
                  local_time.tm_hour, local_time.tm_min, local_time.tm_sec, millis);
}

int qol_log_format(char* out, size_t cap, const char* ts, int64_t frame,
                   QolLogLevel level, QolDomain domain, const char* src, const char* msg) {
    LineBuilder builder(out, cap);
    builder.put_str(ts);
    builder.put(' ');
    builder.put('f');
    builder.put('=');
    builder.put_i64(frame);
    builder.put(' ');
    builder.put(level_char(level));
    builder.put(' ');
    builder.put_str(qol_domain_token(domain));
    builder.put(' ');
    builder.put_str(src);
    builder.put(' ');
    builder.put_msg(msg, level != QolLogLevel::kDebug);
    return builder.finish();
}

void qol_log_write(QolLogLevel level, QolDomain domain, const char* file, int line,
                   const char* fmt, ...) {
    char msg[kMsgCap] = {};
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n < 0) {
        msg[0] = '\0';
    } else if (static_cast<size_t>(n) >= sizeof(msg)) {
        // 溢出：在截断末尾补 "..."，保证 NUL 终止。
        const size_t keep = sizeof(msg) - 4;
        msg[keep] = '.';
        msg[keep + 1] = '.';
        msg[keep + 2] = '.';
        msg[keep + 3] = '\0';
    }
    char src[512];
    std::snprintf(src, sizeof(src), "%s:%d", qol_log_basename(file), line);
    emit_line(level, domain, src, msg);
}

void qol_log_write_kotlin(QolLogLevel level, QolDomain domain, const char* src, const char* msg) {
    // 与 QOL_LOG_DEBUG 宏保持同一门控：debug 关闭时在格式化前短路，
    // 避免 Kotlin backlog 重放把 D 级条目落盘/上 logcat（零开销语义）。
    if (level == QolLogLevel::kDebug && !qol_log_debug_enabled()) return;
    // src 是唯一权威的定位信息：SDK 转发方可能传入 __FILE__ 绝对路径，
    // 落盘前统一压成 basename，避免泄漏构建机路径与出现半截文件名。
    char source[160];
    qol_log_normalize_src(source, sizeof(source), src);
    emit_line(level, domain, source, msg == nullptr ? "" : msg);
}
