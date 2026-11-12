#pragma once

// game 层日志过渡缝（T2l-w-write，seam 方案 D）：调用点沿用 QOL_LOG_* 形态，后端经
// contracts 面 QolHostSdk.log 输出（game→contracts 允许，零 infra include）。
// 先例形态见 plugins/core.item/hooks/inventory_logging（同 contracts 面 + 本地宏重定义）。
// 未注入 SDK 时静默降级（不输出、不崩溃）；节流与去重由 native 统一 logger 内部负责，
// 本缝不维护帧号状态。

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "contracts/host_sdk.h"
#include "game/clock.h"

namespace qol::game {

// 域序数（与 infra/log/qol_log.h 的 QolDomain 对齐；game 层不 include infra 枚举）。
constexpr int32_t kQolLogDomainPlatform = 0;
constexpr int32_t kQolLogDomainCore = 1;
constexpr int32_t kQolLogDomainApi = 3;
constexpr int32_t kQolLogDomainInventory = 5;
constexpr int32_t kQolLogDomainUi = 12;
constexpr int32_t kQolLogDomainCatalog = 14;

// 级别序数（与 QolLogLevel 对齐）：0=Debug、1=Info、2=Warn。
constexpr int32_t kQolLogLevelDebug = 0;
constexpr int32_t kQolLogLevelInfo = 1;
constexpr int32_t kQolLogLevelWarn = 2;
constexpr int32_t kQolLogLevelError = 3;

inline const contracts::QolHostSdk*& qol_seam_sdk_slot() {
    static const contracts::QolHostSdk* sdk = nullptr;
    return sdk;
}

inline void qol_seam_bind_host_sdk(const contracts::QolHostSdk* sdk) {
    qol_seam_sdk_slot() = sdk;
}

inline bool qol_seam_debug_enabled() {
    const auto* sdk = qol_seam_sdk_slot();
    return sdk != nullptr && sdk->log.debug_enabled != nullptr && sdk->log.debug_enabled() != 0;
}

// src 只保留 basename：NDK 构建的 __FILE__ 可能是绝对路径，96 字节缓冲会把长路径
// 截成半截文件名（例如 ".../cpp/plugins/core.i"），日志因此无法定位调用点。
inline const char* qol_seam_basename(const char* path) {
    if (path == nullptr) return "";
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

// 预格式化（printf 语义）后经 SDK 输出；level/domain 序数对齐 infra 枚举
// （kDebug=0、kApi=3），game 层不得 include infra 枚举。
inline void qol_seam_write(int32_t level, int32_t domain, const char* file, int line,
                              const char* fmt, ...) {
    const auto* sdk = qol_seam_sdk_slot();
    if (sdk == nullptr || sdk->log.write == nullptr || file == nullptr || fmt == nullptr) return;
    // debug 关闭时在格式化前短路，避免为诊断日志付出格式化成本。
    if (level == kQolLogLevelDebug && !qol_seam_debug_enabled()) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (written < 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= contracts::kMessageCap
                                ? contracts::kMessageCap
                                : static_cast<uint32_t>(written);
    char source[160]{};
    std::snprintf(source, sizeof(source), "%s:%d", qol_seam_basename(file), line);
    (void)sdk->log.write(level, domain, source, message, length);
}

// 便捷宏：调用点只写域与内容（printf 语义），file/line 由宏注入。
#define QOL_LOG_SEAM_INFO(dom, ...) \
    qol::game::qol_seam_write(qol::game::kQolLogLevelInfo, (dom), __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_SEAM_WARN(dom, ...) \
    qol::game::qol_seam_write(qol::game::kQolLogLevelWarn, (dom), __FILE__, __LINE__, __VA_ARGS__)
#define QOL_LOG_SEAM_ERROR(dom, ...) \
    qol::game::qol_seam_write(qol::game::kQolLogLevelError, (dom), __FILE__, __LINE__, __VA_ARGS__)
// DEBUG 在宏内先判开关再展开参数，未开启时不为诊断数据付出任何构造成本。
// dom 参数只为调用点可读，seam 固定写 api 域。
#define QOL_LOG_SEAM_DEBUG(dom, ...)                                              \
    do {                                                                          \
        if (qol::game::qol_seam_debug_enabled())                                  \
            qol::game::qol_seam_write(qol::game::kQolLogLevelDebug,               \
                                      qol::game::kQolLogDomainApi,                \
                                      __FILE__, __LINE__, __VA_ARGS__);           \
    } while (0)

}  // namespace qol::game
