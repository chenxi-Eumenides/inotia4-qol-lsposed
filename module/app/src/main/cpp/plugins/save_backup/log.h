#pragma once

#include <cstdarg>
#include <cstdio>

#include "../../contracts/host_sdk.h"
#include "feature.h"

namespace qol::features::save_backup::private_log {

inline void write(const contracts::QolHostSdk* sdk, int32_t level, int32_t domain,
                  const char* file, int line, const char* format, ...) {
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0) return;
    const char* source_name = file == nullptr ? "" : file;
    for (const char* p = source_name; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') source_name = p + 1;
    }
    char source[128]{};
    std::snprintf(source, sizeof(source), "%s:%d", source_name, line);
    const uint32_t message_len = static_cast<uint32_t>(length) < contracts::kMessageCap
                                      ? static_cast<uint32_t>(length)
                                      : contracts::kMessageCap;
    (void)sdk->log.write(level, domain, source, message, message_len);
}

}  // namespace qol::features::save_backup::private_log

#define SAVE_BACKUP_LOG_INFO(...) \
    ::qol::features::save_backup::private_log::write(                              \
        save_backup_host_sdk(), 1, 8, __FILE__, __LINE__, \
        __VA_ARGS__)
#define SAVE_BACKUP_LOG_WARN(...) \
    ::qol::features::save_backup::private_log::write(                              \
        save_backup_host_sdk(), 2, 8, __FILE__, __LINE__, \
        __VA_ARGS__)
#define SAVE_BACKUP_LOG_DEBUG(...)                                                   \
    do {                                                                             \
        const auto* _save_backup_sdk =                                              \
            save_backup_host_sdk();                                                  \
        if (_save_backup_sdk != nullptr && _save_backup_sdk->log.debug_enabled != nullptr && \
            _save_backup_sdk->log.debug_enabled() != 0) {                           \
            ::qol::features::save_backup::private_log::write(                      \
                _save_backup_sdk, 0, 8, __FILE__, __LINE__, __VA_ARGS__);            \
        }                                                                            \
    } while (0)
