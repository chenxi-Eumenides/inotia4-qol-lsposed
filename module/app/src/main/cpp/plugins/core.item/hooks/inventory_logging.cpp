#include "inventory_logging.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

const qol::contracts::QolHostSdk* g_sdk = nullptr;

// src 只保留 basename：__FILE__ 在 NDK 构建中是绝对路径，96 字节缓冲会把长路径
// 截成半截文件名，日志因此无法定位调用点。
const char* log_basename(const char* path) {
    if (path == nullptr) return "";
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

}

void inventory_log_bind_sdk(const qol::contracts::QolHostSdk* sdk) { g_sdk = sdk; }

bool inventory_log_debug_enabled() {
    return g_sdk != nullptr && g_sdk->log.debug_enabled != nullptr &&
           g_sdk->log.debug_enabled() != 0;
}

bool inventory_log_debug_due(int64_t* last_frame, int64_t every_frames) {
    if (last_frame == nullptr || every_frames <= 0) {
        return true;
    }
    if (*last_frame + 1 < every_frames) {
        ++*last_frame;
        return false;
    }
    *last_frame = 0;
    return true;
}

void inventory_log_write(int32_t level, const char* file, int line, const char* format, ...) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || file == nullptr || format == nullptr) {
        return;
    }
    std::array<char, qol::contracts::kMessageCap + 1> message{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message.data(), message.size(), format, args);
    va_end(args);
    if (written < 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= qol::contracts::kMessageCap
        ? qol::contracts::kMessageCap
        : static_cast<uint32_t>(written);
    char source[160]{};
    std::snprintf(source, sizeof(source), "%s:%d", log_basename(file), line);
    (void)g_sdk->log.write(level, 5, source, message.data(), length);
}
