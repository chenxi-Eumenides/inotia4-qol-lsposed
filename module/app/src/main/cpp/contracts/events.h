#pragma once

// 事件载荷契约（ARCHITECTURE.md §3.5 配置变更广播、§6.3 事件总线）。
// 类型 id 为编译期常量；payload 语义：变更后的值，key 为静态字符串。

#include <cstdint>

namespace qol::contracts {

enum : uint32_t {
    kEventConfigChanged = 1,
    // 2 已退役（原存档进入事件），不得复用；存档就绪改由事件状态
    // kEventStateSaveReady 表达。
    kEventSaveExit = 3,
    // 能力注册表变更：注册成功与注销都会发布，消费者收到后重新 require。
    kEventCapabilityChanged = 5,
};

struct ConfigChangedEvent {
    const char* key;   // 静态字符串
    int32_t int_value; // 变更后的值
};

struct SaveExitEvent {
    int32_t slot;
};

// 能力注册表变更通知（§3.4.4）。只描述能力身份与可用性，
// 不携带接口指针：消费者收到后须重新 require，避免缓存越过撤销点的指针。
struct CapabilityChangedEvent {
    const char* id;       // 静态字符串，回调期间有效
    uint16_t major;
    uint16_t minor;
    uint8_t available;    // 1=注册成功，0=已注销
    uint8_t reserved[3];
};

// 事件状态：所有者声明 + 更新，宿主在帧点统一派发。event id 为编译期常量。
constexpr uint32_t kEventStateSaveReady = 4;
constexpr uint16_t kEventStateSaveReadySchema = 1;

enum class EventStateAvailability : uint8_t { kUnavailable = 0, kAvailable = 1 };
enum class EventStateChangeKind : uint8_t {
    kSnapshot = 0,
    kBecameReady = 1,
    kCeasedReady = 2,
    kUnavailable = 3,
};

struct EventStateDescriptor {
    uint32_t size;
    uint32_t event_id;
    uint16_t schema_version;
    uint16_t reserved;
};

struct EventStateValue {
    uint32_t size;
    uint32_t event_id;
    uint16_t schema_version;
    uint8_t availability;
    uint8_t value;
    int32_t slot;
    uint64_t revision;
};

struct EventStateChange {
    uint32_t size;
    uint32_t event_id;
    uint8_t change_kind;
    uint8_t availability;
    uint8_t value;
    uint8_t reserved;
    int32_t slot;
    uint64_t revision;
};

using QolEventStateHandler = void (*)(void* ctx, const EventStateChange* change);

}  // namespace qol::contracts
