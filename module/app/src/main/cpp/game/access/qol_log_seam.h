#pragma once

// game 层日志过渡缝（T2l-w-write，seam 方案 D）：调用点沿用 QOL_LOG_* 形态，后端经
// contracts 面 QolHostSdk.log 输出（game→contracts 允许，零 infra include）。
// 先例形态见 plugins/inventory_hooks/inventory_logging（同 contracts 面 + 本地宏重定义）。
// 未注入 SDK 时静默降级（不输出、不崩溃）；帧节流语义与 infra QolLogThrottle::due 帧分支一致。

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "contracts/host_sdk.h"
#include "game/clock.h"

namespace qol::game {

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

// 帧节流：与 infra QolLogThrottle::due 的 frame>=0 分支逐行等价（首次放行 / 帧回退重置 /
// frame-last>=every）。frame<0（帧号未就绪）按首次放行——现状该路径由帧任务体执行、
// frame 恒 >=0，属不可达防御（登记执行记录）。
inline bool qol_seam_due(int64_t* last_frame, int64_t every_frames) {
    if (last_frame == nullptr || every_frames <= 0) return true;
    const int64_t frame = qol::game::frame_count();
    if (frame < 0) return true;
    if (*last_frame == INT64_MIN) {
        *last_frame = frame;
        return true;
    }
    if (frame < *last_frame) {
        *last_frame = frame;
        return true;
    }
    if (frame - *last_frame >= every_frames) {
        *last_frame = frame;
        return true;
    }
    return false;
}

// 预格式化（printf 语义）后经 SDK 输出；level/domain 序数对齐 infra 枚举
// （kDebug=0、kApi=3），game 层不得 include infra 枚举。
inline void qol_seam_write(int32_t level, const char* file, int line, const char* fmt, ...) {
    const auto* sdk = qol_seam_sdk_slot();
    if (sdk == nullptr || sdk->log.write == nullptr || file == nullptr || fmt == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (written < 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= contracts::kMessageCap
                                ? contracts::kMessageCap
                                : static_cast<uint32_t>(written);
    char source[96]{};
    std::snprintf(source, sizeof(source), "%s:%d", file, line);
    (void)sdk->log.write(level, 3 /* QolDomain::kApi */, source, message, length);
}

}  // namespace qol::game
