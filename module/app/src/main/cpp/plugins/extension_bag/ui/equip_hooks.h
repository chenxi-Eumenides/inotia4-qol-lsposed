#pragma once

#include <cstdint>

#include "contracts/inventory_refresh.h"
#include "contracts/plugin.h"

// 原版装备页 5 条 UI 入口 hook（自 core.item/hooks/inventory_hooks 迁入）。
//
// 资源声明归 EB：本 TU 提供 5 条 claim（由 assembly/claims.cpp 拼入 EB 总表）；
// wrapper 业务体在 ui/equip_hooks.cpp。分流语义：
// - ButtonEquipExe / ButtonUnequipExe / OKConfirmUseItem / EquipControlEventProc
//   先按扩展物品处理，未命中转原版 backup；
// - RefreshItemArea 是 EB 自身刷新路径的入口，同时充当 inventory_refresh 的
//   call_original trampoline（backup 槽）。
namespace qol::features::extension_bag::equip_hooks {

// 5 条入口 hook 声明（静态存储，进程生命周期）；out_count 可空。
const qol::contracts::ResourceClaim* equip_hooks_claims(uint32_t* out_count);

// 宿主 on_resource_ready：按资源 id 把原函数 backup 写入 ui_state 的 ui backup 区。
// RefreshItemArea 的 backup 槽同时是 inventory_refresh.call_original 的 trampoline。
// 未命中/空 backup 返回 1，由宿主按 fail-closed 回滚。
int32_t bind_backup(const char* resource_id, void* backup);

// inventory_refresh.call_original：直调 RefreshItemArea 的 backup trampoline，
// 不重新进入 wrapper（回入会重复加锁）。返回 true = 已调原版。
bool call_refresh_item_area_original();

}  // namespace qol::features::extension_bag::equip_hooks

// Host 装配期注入原版 PLAYER_UpdateShortcut（inventory_refresh.update_shortcut_category）；
// 原版函数按类别刷新快捷栏，不经入口 hook。
void extension_bag_bind_player_update_shortcut(void (*update_shortcut)(int32_t));

namespace qol::features::extension_bag {

// inventory_refresh provider（owner=extension_bag）。core.item 的出售结算等路径经
// capability 请求原函数刷新，避免跨插件直接符号依赖。
extern const qol::contracts::InventoryRefreshApi kInventoryRefreshApi;

}  // namespace qol::features::extension_bag
