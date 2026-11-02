#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "plugins/extension_bag/runtime/state.h"

// src 只保留 basename：__FILE__ 可能是绝对路径，绝对路径会泄漏构建机目录，
// 且超出宿主 src 缓冲后会被截成半截文件名而无法定位调用点。
inline const char* extension_bag_log_basename(const char* path) {
    if (path == nullptr) return "";
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

inline void extension_bag_log(int32_t level, const char* file, int line, const char* format, ...) {
    const qol::contracts::QolHostSdk* sdk = runtime_state().host.sdk;
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    // debug 关闭时在格式化前短路：逐对象/逐帧 DEBUG 明细不能为落盘失败付出格式化成本。
    if (level == 0 && (sdk->log.debug_enabled == nullptr || sdk->log.debug_enabled() == 0)) return;
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
    std::snprintf(source, sizeof(source), "%s:%d", extension_bag_log_basename(file), line);
    (void)sdk->log.write(level, 6, source, message, length);
}

// 统一日志系统 P2/P3：扩展背包所有日志经统一 native 日志系统单写者输出。
// file/line 由宏注入：domain 参数仅为调用点可读性保留，实际域固定为 extension_bag。
#define QOL_LOG_INFO(domain, ...) extension_bag_log(1, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_WARN(domain, ...) extension_bag_log(2, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_ERROR(domain, ...) extension_bag_log(3, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_DEBUG(domain, ...) extension_bag_log(0, __FILE__, __LINE__, __VA_ARGS__)
#define VIRTBAG_LOG(...) extension_bag_log(1, __FILE__, __LINE__, __VA_ARGS__)
#define VIRTBAG_LOG_WARN(...) extension_bag_log(2, __FILE__, __LINE__, __VA_ARGS__)
#define VIRTBAG_LOG_ERROR(...) extension_bag_log(3, __FILE__, __LINE__, __VA_ARGS__)
#define VIRTBAG_LOG_DEBUG(...) extension_bag_log(0, __FILE__, __LINE__, __VA_ARGS__)
