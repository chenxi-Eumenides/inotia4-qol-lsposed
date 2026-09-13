#pragma once

#include "feature/autosell/autosell_rules.h"
#include "game_state.h"

// 把数据层物品引用转成纯规则视图（阶段 B）。
//
// 依赖游戏内存与符号（item_is_equip / fn_get_rarity / 特殊谓词），
// 故不编 host 单测；纯判定仍由 autosell::should_sell 承担。

// 构建物品视图；ref 无原生物品、category<=0 或 out 为空返回 false（跳过）。
bool autosell_build_view(const InventoryItemRef& ref, autosell::ItemView* out);
