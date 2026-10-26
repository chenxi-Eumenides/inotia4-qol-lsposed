#pragma once

#include <cstdint>

// 库存刷新能力契约（ARCHITECTURE.md §3.4；D2c-2b-1，R-32 刷新路径）。
//
// 定位：把 `plugins/core.item/inventory` 持有的 raw-original refresh trampoline 暴露为能力，
// 供 `plugins/extension_bag` 等消费者经能力注册表获取，切断 plugins↔plugins 直接符号
// 依赖（原先 include native_inventory_hook.h 或前置声明）。
//
// 语义：`call_original` 经 backup trampoline 直调原版 `UIEquip_RefreshItemArea`，绕过
// 入口 hook 避免回入 wrapper（回入会重复加锁）；返回 true = 已调用（存在可用原函数），
// false = 无可用原函数（未刷新，调用方按 no-op 降级）。`update_shortcut` 直调绑定的原版
// `PLAYER_UpdateShortcut`，不经过 hook。两个函数仅游戏主线程调用。
//
// 契约类型限制同 contracts/plugin.h：POD + 固定宽度 C 类型 + 函数指针；禁止 STL。

namespace qol::contracts {

constexpr const char* kInventoryRefreshCapabilityId = "inventory_refresh";
constexpr int32_t kInventoryRefreshVersionMajor = 1;
constexpr int32_t kInventoryRefreshVersionMinor = 2;

struct InventoryRefreshApi {
    bool (*call_original)();
    // 兼容字段：原版 PLAYER_UpdateShortcut 按类别刷新，无参调用等于用未定义 w0，
    // 仅保留给旧消费者；新代码必须使用 update_shortcut_category。
    void (*update_shortcut)();
    // 按类别重算快捷栏中该类别物品的数量（直接调用原版函数，不经入口 hook）。
    void (*update_shortcut_category)(int32_t category);
};

}  // namespace qol::contracts
