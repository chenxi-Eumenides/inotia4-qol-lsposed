#pragma once

// 能力契约版本与兼容规则（ARCHITECTURE.md §3.4）：
// major 必须相等；提供方 minor ≥ 消费者声明值。不满足即拒绝（blocked）。

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QolVersion {
    uint16_t major;
    uint16_t minor;
} QolVersion;

typedef struct QolVersionRange {
    uint16_t major;     // 必须与提供方相等
    uint16_t min_minor; // 提供方 minor 必须 ≥ 本值
} QolVersionRange;

#ifdef __cplusplus
}  // extern "C"

namespace qol::contracts {

constexpr bool satisfies(QolVersion provided, QolVersionRange required) {
    return provided.major == required.major && provided.minor >= required.min_minor;
}

}  // namespace qol::contracts
#endif
