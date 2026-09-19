#pragma once

#include "rules.h"
#include "read_only.h"

// 把数据层物品引用转成纯规则视图（阶段 B）。
//
// 依赖 Game Host 物品只读面；宝石属性位仍由原生物品布局读取，故不编 host 单测；
// 纯判定仍由 autosell::should_sell 承担。

// 构建物品视图；ref 无原生物品、category<=0 或 out 为空返回 false（跳过）。
bool autosell_build_view(const qol::features::autosell::read_only::InventoryItemRef& ref,
                         autosell::ItemView* out);
