#pragma once

// core.save 插件本地日志 seam（分层约束：插件只能 include contracts/ 与自身目录，
// 不得依赖 infra/log）。经 plugin.cpp 安装的 bridge 转发到 sdk->log.write，
// domain 固定取 kSave=7（与 infra/log/qol_log.h 的 QolDomain 枚举序一致）。

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace qol::features::core_save {

using CoreSaveLogSink = void (*)(int32_t level, const char* source, const char* message,
                                 uint32_t length);

// 跨 TU 单一实体（C++17 inline 变量）：plugin.cpp 安装 bridge，save_transition.cpp /
// slot_data_provider.cpp 消费同一 sink；函数内 static 会每 TU 各一份，不可用。
inline CoreSaveLogSink g_core_save_log_sink = nullptr;

inline void core_save_log_install(CoreSaveLogSink fn) { g_core_save_log_sink = fn; }

// 与宿主 QolDomain 枚举序一致（kPlatform=0 … kExtensionBag=6, kSave=7）；插件不得
// include infra/log，取值以本地常量承载，bridge 落盘时使用。
inline constexpr int32_t kCoreSaveLogDomain = 7;

// src 只保留 basename：__FILE__ 可能是绝对路径，绝对路径会泄漏构建机目录，
// 且超出宿主 src 缓冲后会被截成半截文件名而无法定位调用点。
inline const char* core_save_log_basename(const char* path) {
    if (path == nullptr) return "";
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

// sink 未装配（未 install / 插件未启用）时静默丢弃，不落盘。
inline void core_save_log(int32_t level, const char* file, int line, const char* format, ...) {
    const CoreSaveLogSink sink = g_core_save_log_sink;
    if (sink == nullptr || format == nullptr) return;
    char message[1024]{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written <= 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= sizeof(message)
                                ? static_cast<uint32_t>(sizeof(message) - 1)
                                : static_cast<uint32_t>(written);
    char source[160]{};
    std::snprintf(source, sizeof(source), "%s:%d", core_save_log_basename(file), line);
    sink(level, source, message, length);
}

// 按毫秒节流：调用点频率固定（每帧 / 每 30 帧），与帧率无关地限制 WARN 落盘频率。
// steady_clock 单调，无帧号回绕问题；last_ms == 0 视为首次调用，立即放行。
struct CoreSaveLogThrottle {
    int64_t last_ms = 0;
    bool due(int64_t min_interval_ms) {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        if (last_ms != 0 && now - last_ms < min_interval_ms) return false;
        last_ms = now;
        return true;
    }
};

}  // namespace qol::features::core_save

// level 与宿主一致：0=debug、1=info、2=warn、3=error。domain 参数只为调用点可读性
// 保留，实际域固定为 kSave（bridge 内写 kCoreSaveLogDomain）。
#define CS_LOG_INFO(domain, ...) \
    ::qol::features::core_save::core_save_log(1, __FILE__, __LINE__, __VA_ARGS__)
#define CS_LOG_WARN(domain, ...) \
    ::qol::features::core_save::core_save_log(2, __FILE__, __LINE__, __VA_ARGS__)
