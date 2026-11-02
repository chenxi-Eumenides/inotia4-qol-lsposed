#pragma once

// 事件载荷契约（ARCHITECTURE.md §3.5 配置变更广播、§6.3 事件总线）。
// 类型 id 为编译期常量；payload 语义：变更后的值，key 为静态字符串。

#include <cstdint>

namespace qol::contracts {

enum : uint32_t {
    kEventConfigChanged = 1,
    kEventSaveEnter = 2,
    kEventSaveExit = 3,
};

struct ConfigChangedEvent {
    const char* key;   // 静态字符串
    int32_t int_value; // 变更后的值
};

struct SaveEnterEvent {
    int32_t slot;
};

struct SaveExitEvent {
    int32_t slot;
};

}  // namespace qol::contracts
