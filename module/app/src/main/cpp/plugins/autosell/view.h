#pragma once

#include "rules.h"
#include "read_only.h"
#include "../../contracts/attribute_quality.h"

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
    int category = 0;
    bool is_backpack = false;
    int badge_rank = 0;
    int dice_tier = 0;
};

using AttributeQualityApiProvider =
    const qol::contracts::AttributeQualityApi* (*)();

// 把数据层物品引用转成纯规则视图（阶段 B）。
//
// 依赖 Game Host 物品只读面；宝石质量接口由调用方注入 provider，宿主依赖不进入本层；
// 纯判定仍由 autosell::should_sell 承担。

// 构建物品视图；失败原因用于 autosell 扫描诊断，不改变跳过该物品的行为。
AutosellViewBuildReason autosell_build_view(
    const qol::features::autosell::read_only::InventoryItemRef& ref,
    autosell::ItemView* out, AutosellViewDiagnostics* diagnostics = nullptr,
    AttributeQualityApiProvider quality_provider = nullptr);
