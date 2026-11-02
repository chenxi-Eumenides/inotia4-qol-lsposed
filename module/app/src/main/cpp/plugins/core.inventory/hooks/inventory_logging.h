#pragma once

#include <cstdint>

#include "contracts/host_sdk.h"

void core_inventory_log_bind_sdk(const qol::contracts::QolHostSdk* sdk);
bool core_inventory_log_debug_enabled();
bool core_inventory_log_debug_due(int64_t* last_frame, int64_t every_frames);
void core_inventory_log_write(int32_t level, const char* file, int line, const char* format, ...);

#define QOL_LOG_INFO(domain, ...) \
    core_inventory_log_write(1, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_WARN(domain, ...) \
    core_inventory_log_write(2, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_ERROR(domain, ...) \
    core_inventory_log_write(3, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_DEBUG(domain, ...)                                  \
    do {                                                            \
        if (core_inventory_log_debug_enabled()) {                   \
            core_inventory_log_write(0, __FILE__, __LINE__, __VA_ARGS__); \
        }                                                           \
    } while (0)
#define QOL_LOG_DEBUG_EVERY(domain, every_frames, ...)                                  \
    do {                                                                                 \
        static int64_t _core_inventory_last_frame = 0;                                  \
        if (core_inventory_log_debug_enabled() &&                                       \
            core_inventory_log_debug_due(&_core_inventory_last_frame, (every_frames))) { \
            core_inventory_log_write(0, __FILE__, __LINE__, __VA_ARGS__);               \
        }                                                                                \
    } while (0)
