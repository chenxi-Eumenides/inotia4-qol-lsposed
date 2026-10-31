#pragma once

#include <cstdint>

#include "../../../contracts/host_sdk.h"
#include "../../../contracts/game_host.h"
#include "../../../contracts/item_facts.h"
#include "../../../contracts/s2_writeback.h"

// 使用 LSPosed Native Hook API 提供的 backup；扩展对象由逻辑库存适配器处理，原版对象回退到 backup。

// 模块主动刷新必须走 backup/trampoline，避免经被 Hook 地址回入 wrapper 后重复加锁。
// core.item 实现：raw-original refresh trampoline 经 `inventory_refresh` capability 暴露给消费者；
// 返回 true = 已调原版（存在 trampoline），false = 无可用原函数。
bool inventory_native_hook_call_refresh_item_area_original();
void inventory_native_hook_bind_player_update_shortcut(void (*update_shortcut)(int32_t));
void inventory_native_hook_update_shortcut_category(int32_t category);
void inventory_native_hook_update_shortcut();
bool inventory_native_hook_bind_sdk(const qol::contracts::QolHostSdk* sdk);
bool inventory_native_hook_bind_game(const qol::contracts::QolGameHost* game);
bool inventory_native_hook_bind_item_facts(const qol::contracts::ItemFactsApi* facts);
void inventory_native_hook_set_support(bool enabled);
bool inventory_native_hook_support_enabled();

// ---------------------------------------------------------------------------
// S2 写侧进位/借位纯函数已单源化至 contracts/s2_writeback.h（R-45/R-49），
// core.item 与 core.inventory include 同一份。
// ---------------------------------------------------------------------------

namespace qol::features::inventory_hooks {

// 给 detached 句柄写 canonical 数量（inventory.action.create 的 count>1 路径）。
// 类别不可堆叠返回 Unsupported；句柄失效或数量编码不可用返回 NotFound/NotReady。
QolResult write_detached_item_count(const qol::contracts::QolItemHandle& item, uint32_t count);

}  // namespace qol::features::inventory_hooks
