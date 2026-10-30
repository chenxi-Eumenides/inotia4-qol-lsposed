// qol_log.h —— 统一日志系统 P0 基础设施（native 侧）接口契约。
//
// 依赖方向：本文件仅依赖 STL；core 不得依赖 feature/bridge。
// 行格式唯一权威：`<ts> f=<frame> <L> <domain> <src> <msg>`（单空格分隔）。
//
// 噪声上限（写入侧强制，调用点无法绕过）：
// - INFO 每域每帧最多 kQolLogInfoPerFrameCap 行；超出时先输出一行封顶标记，随后丢弃
//   该域本帧剩余 INFO。DEBUG/WARN/ERROR 不受限（DEBUG 已有全局开关）。
// - 文件 sink 超过 kQolLogFileCapBytes 时轮转为 `qol.log.1`（只保留一份旧文件）。
#pragma once

#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>

enum class QolLogLevel : uint8_t { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

enum class QolDomain : uint8_t {
    kPlatform = 0, kCore, kHttp, kApi, kOp, kInventory, kExtensionBag,
    kSave, kSaveBackup, kAutosell, kCraft, kAttrRange, kUi, kConfig, kCatalog,
    kCustomRecipe,
    kWorldTuning,
    kSpecialEquip,
};

// INFO 单帧单域行数上限；超出后只留一行封顶标记。
constexpr uint32_t kQolLogInfoPerFrameCap = 16;
// 文件 sink 单文件字节上限；写满后轮转为 `<path>.1`。
constexpr int64_t kQolLogFileCapBytes = 4 * 1024 * 1024;

const char* qol_domain_token(QolDomain domain);          // 小写 token，见实现表
QolDomain   qol_domain_from_token(const char* token);    // 未知返回 kPlatform

const char* qol_log_basename(const char* path);          // 取 '\\' 或 '/' 之后部分

// 落盘 src 归一化：只保留最后一个 '/' 或 '\\' 之后的部分。
// 目的：SDK 转发方可能把 __FILE__ 的绝对构建路径写进 src；绝对路径既泄漏构建机
// 目录结构，也会在转发方自身的 src 缓冲里被截断成半截文件名而无法定位。
// 缓冲区不足时按 cap-1 截断并保证 NUL；返回完整所需长度（不含 NUL）。
int  qol_log_normalize_src(char* out, size_t cap, const char* src);

using QolFrameProvider = int64_t (*)();
void    qol_log_set_frame_provider(QolFrameProvider fn); // fn=null → frame=-1
int64_t qol_log_current_frame();

bool qol_log_debug_enabled();
void qol_log_set_debug_enabled(bool enabled);

void qol_log_init();                 // 打开文件 sink（截断模式）并写起始横幅；幂等；任意线程
bool qol_log_file_ready();

// 纯格式化（host 可测，无 I/O）
void qol_log_format_ts(char* out, size_t cap, int64_t ts_ms); // "YYYY-MM-DD HH:MM:SS.mmm" 本地时间
// 转义规则：level != kDebug 时把 '\n' 转成字面 "\\n"、丢弃 '\r'；kDebug 保留真实换行。
// 返回写入字节数（不含 NUL）；缓冲区不足时返回所需长度（>cap）且内容已截断。
int  qol_log_format(char* out, size_t cap, const char* ts, int64_t frame,
                    QolLogLevel level, QolDomain domain, const char* src, const char* msg);

// 发射：logcat + 文件单写者
void qol_log_write(QolLogLevel level, QolDomain domain, const char* file, int line,
                   const char* fmt, ...) __attribute__((format(printf, 5, 6)));
// Kotlin 转发入口：src 已含 "File.kt:line"，msg 为完整正文（不格式化）
void qol_log_write_kotlin(QolLogLevel level, QolDomain domain, const char* src, const char* msg);

#if !defined(__ANDROID__)
// Host-only test seam: receives the fully formatted line from emit_line before the
// no-op Android stub. Not compiled into Android builds.
using QolLogHostSink = void (*)(QolLogLevel level, QolDomain domain, const char* line);
void qol_log_set_host_sink(QolLogHostSink sink);
#endif

// 限流原语（P3 会大量使用，本次一并提供并测试）
struct QolLogThrottle {
    int64_t last_frame = INT64_MIN;   // 注意：需要 <climits>
    int64_t last_ms = 0;
    bool due(int64_t every_frames);
};
struct QolLogChangeGate {
    int64_t last = INT64_MIN;
    bool changed(int64_t value);
};

#define QOL_LOG_INFO(dom, ...)  qol_log_write(QolLogLevel::kInfo,  (dom), __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_WARN(dom, ...)  qol_log_write(QolLogLevel::kWarn,  (dom), __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_ERROR(dom, ...) qol_log_write(QolLogLevel::kError, (dom), __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_DEBUG(dom, ...) \
    do { if (qol_log_debug_enabled()) qol_log_write(QolLogLevel::kDebug, (dom), __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define QOL_LOG_DEBUG_EVERY(dom, every_frames, ...) \
    do { static QolLogThrottle _qlt; \
         if (qol_log_debug_enabled() && _qlt.due(every_frames)) \
             qol_log_write(QolLogLevel::kDebug, (dom), __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define QOL_LOG_DEBUG_ON_CHANGE(dom, gate, value, ...) \
    do { if (qol_log_debug_enabled() && (gate).changed(value)) \
             qol_log_write(QolLogLevel::kDebug, (dom), __FILE__, __LINE__, __VA_ARGS__); } while (0)
