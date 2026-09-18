#pragma once

#include <cstdint>

// 库存刷新能力契约（ARCHITECTURE.md §3.4；D2c-2b-1，R-32 刷新路径）。
//
// 定位：把 `plugins/inventory_hooks` 持有的 raw-original refresh trampoline 暴露为能力，
// 供 `plugins/extension_bag` / `feature/patch` 等消费者经能力注册表获取，切断
// plugins↔plugins 直接符号依赖（原先 include native_inventory_hook.h 或前置声明）。
//
// 语义：`call_original` 经 backup trampoline 直调原版 `UIEquip_RefreshItemArea`，绕过
// 入口 hook 避免回入 wrapper（回入会重复加锁）；返回 true = 已调用（存在可用原函数），
// false = 无可用原函数（未刷新，调用方按 no-op 降级）。仅游戏主线程调用。
//
// 契约类型限制同 contracts/plugin.h：POD + 固定宽度 C 类型 + 函数指针；禁止 STL。

namespace qol::contracts {

constexpr const char* kInventoryRefreshCapabilityId = "inventory_refresh";
constexpr int32_t kInventoryRefreshVersionMajor = 1;
constexpr int32_t kInventoryRefreshVersionMinor = 0;

struct InventoryRefreshApi {
    bool (*call_original)();
};

}  // namespace qol::contracts
