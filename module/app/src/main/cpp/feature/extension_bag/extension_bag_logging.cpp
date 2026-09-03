#include "feature/extension_bag/extension_bag_logging.h"

#include <android/log.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace {

constexpr char kVirtBagLogPath[] =
    "/sdcard/Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files/inotia4-export.log";
std::mutex g_virtbag_log_mtx;

}

void extension_bag_log(const char* format, ...) {
    char message[2048] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, VIRTBAG_TAG, "%s", message);

    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_r(&seconds, &local_time);
    const int millisecond_part = static_cast<int>(milliseconds % 1000);
    std::lock_guard<std::mutex> lock(g_virtbag_log_mtx);
    const int fd = open(kVirtBagLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    dprintf(fd, "%04d-%02d-%02d %02d:%02d:%02d.%03d [N] %s\n",
            local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
            local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
            millisecond_part, message);
    close(fd);
}
