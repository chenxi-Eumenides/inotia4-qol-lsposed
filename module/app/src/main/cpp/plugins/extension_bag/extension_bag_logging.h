#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "extension_bag_bridge.h"

inline void extension_bag_log(int32_t level, const char* format, ...) {
    const qol::contracts::QolHostSdk* sdk =
        qol::features::extension_bag::extension_bag_host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[1024]{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written <= 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= sizeof(message)
                                ? static_cast<uint32_t>(sizeof(message) - 1)
                                : static_cast<uint32_t>(written);
    (void)sdk->log.write(level, 6, "extension_bag", message, length);
}

// 统一日志系统 P2/P3：扩展背包所有日志经统一 native 日志系统单写者输出。
// 保留 VIRTBAG_LOG 同名宏（355 处调用点无需改动）；级别沿用迁移前语义（原
// extension_bag_log 恒 INFO）。需要更高/更低级别时使用对应变体。
#define QOL_LOG_INFO(domain, ...) extension_bag_log(1, __VA_ARGS__)
#define QOL_LOG_WARN(domain, ...) extension_bag_log(2, __VA_ARGS__)
#define QOL_LOG_ERROR(domain, ...) extension_bag_log(3, __VA_ARGS__)
#define QOL_LOG_DEBUG(domain, ...) extension_bag_log(0, __VA_ARGS__)
#define VIRTBAG_LOG(...) extension_bag_log(1, __VA_ARGS__)
#define VIRTBAG_LOG_WARN(...) extension_bag_log(2, __VA_ARGS__)
#define VIRTBAG_LOG_ERROR(...) extension_bag_log(3, __VA_ARGS__)
#define VIRTBAG_LOG_DEBUG(...) extension_bag_log(0, __VA_ARGS__)
