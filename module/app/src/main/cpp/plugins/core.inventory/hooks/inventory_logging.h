#pragma once

#include "contracts/host_sdk.h"

void core_inventory_log_bind_sdk(const qol::contracts::QolHostSdk* sdk);
bool core_inventory_log_debug_enabled();
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
