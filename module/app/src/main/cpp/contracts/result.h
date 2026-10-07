#pragma once

// 跨边界统一结果类型（ARCHITECTURE.md §3.4.2）。
// 约束：POD + 固定宽度类型，可同时用于内部 C++ 调用与外部 C ABI；
// message 由返回方持有、调用方只读，跨边界不得释放；UTF-8，无 NUL 要求。

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QolResult {
    int32_t ok;          // 0 = 成功；非 0 = 错误码（见下）
    int32_t code;        // 域错误码（成功时 0）
    const char* message; // UTF-8，返回方持有，只读；可为 nullptr
    uint32_t message_len; // 字节数；建议 ≤512，超出由返回方截断
} QolResult;

enum {
    kQolOk = 0,
    kQolErrInvalidArgument = 1,
    kQolErrNotFound = 2,
    kQolErrNotReady = 3,
    kQolErrUnsupported = 4,
    kQolErrConflict = 5,
    kQolErrInternal = 6,
    kQolErrTimeout = 7,
    kQolErrCancelled = 8,
    kQolErrBlocked = 9,
    kQolErrBusy = 10,
};

#ifdef __cplusplus
}  // extern "C"

namespace qol::contracts {

constexpr uint32_t kMessageCap = 512;

inline QolResult ok() { return QolResult{kQolOk, 0, nullptr, 0}; }

inline QolResult error(int32_t code, const char* message, uint32_t length) {
    return QolResult{code, code, message, length};
}

inline bool succeeded(const QolResult& result) { return result.ok == kQolOk; }

}  // namespace qol::contracts
#endif
