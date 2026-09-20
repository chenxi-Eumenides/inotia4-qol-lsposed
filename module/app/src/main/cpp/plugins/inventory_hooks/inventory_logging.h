#pragma once

#include <cstdint>

#include "../../contracts/host_sdk.h"

void inventory_log_bind_sdk(const qol::contracts::QolHostSdk* sdk);
bool inventory_log_debug_enabled();
bool inventory_log_debug_due(int64_t* last_frame, int64_t every_frames);
void inventory_log_write(int32_t level, const char* file, int line, const char* format, ...);

#define QOL_LOG_INFO(domain, ...) \
    inventory_log_write(1, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_WARN(domain, ...) \
    inventory_log_write(2, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_ERROR(domain, ...) \
    inventory_log_write(3, __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_DEBUG_EVERY(domain, every_frames, ...)                                  \
    do {                                                                                 \
        static int64_t _inventory_last_frame = 0;                                       \
        if (inventory_log_debug_enabled() &&                                           \
            inventory_log_debug_due(&_inventory_last_frame, (every_frames))) {          \
            inventory_log_write(0, __FILE__, __LINE__, __VA_ARGS__);                    \
        }                                                                                \
    } while (0)
