#pragma once

#include "rules.h"
#include "read_only.h"

enum class AutosellViewBuildReason : uint8_t {
    kNotAttempted,
    kBuilt,
    kNullOutput,
    kInvalidHandle,
    kInvalidCategory,
    kFactsUnavailable,
    kMissingRequiredFields,
    kJewelPredicateUnavailable,
    kInvalidJewelCategory,
};

struct AutosellViewDiagnostics {
    uint32_t fields = 0;
    int rarity = -1;
    int equip = -1;
    int jewel = -1;
    uint32_t special_types = 0;
};

// 把数据层物品引用转成纯规则视图（阶段 B）。
//
// 依赖 Game Host 物品只读面；宝石属性位仍由原生物品布局读取，故不编 host 单测；
// 纯判定仍由 autosell::should_sell 承担。

// 构建物品视图；失败原因用于 autosell 扫描诊断，不改变跳过该物品的行为。
AutosellViewBuildReason autosell_build_view(
    const qol::features::autosell::read_only::InventoryItemRef& ref,
    autosell::ItemView* out, AutosellViewDiagnostics* diagnostics = nullptr);
