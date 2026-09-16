#pragma once

// 类型化事件总线（ARCHITECTURE.md §6.3）：发布者不依赖订阅者；
// 事件在游戏主线程派发；订阅与注销成对，插件 deactivate 时强制清空。

#include <cstdint>

namespace qol::infra {

using EventTypeId = uint32_t;    // 由事件契约（contracts/）定义的编译期常量
using SubscriptionId = uint64_t; // 0 = 无效

using EventHandler = void (*)(void* ctx, const void* payload);

// 任意线程可调用（典型：装配期与主线程）；返回 0 表示参数非法或已满。
SubscriptionId subscribe(EventTypeId type, EventHandler handler, void* ctx);

bool unsubscribe(SubscriptionId id);

// 主线程调用；按订阅顺序调用处理器，返回被调用的处理器数量。
// 实现按订阅快照派发：派发期间注销的订阅在本轮仍可能被调用一次，下一轮生效。
uint32_t publish(EventTypeId type, const void* payload);

// 清空全部订阅（插件 deactivate 时调用）。
void clear_all();

// 测试用。
void events_reset_for_test();
uint32_t subscription_count();

}  // namespace qol::infra
