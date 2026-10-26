// autosell_view.cpp —— 数据层物品引用 -> 纯规则视图（阶段 B）。

#include "view.h"

#include <cstdint>

namespace {

// 宝石 category 起档（IsJewel 闭区间 28..32；档位 = category - 28）。
constexpr int kJewelCategoryFirst = 28;
constexpr int kBackpackCategoryFirst = 1;
constexpr int kBackpackCategoryLast = 4;
constexpr int kBadgeRankOneFirst = 42;
constexpr int kBadgeRankOneLast = 47;
constexpr int kBlessedBadgeCategory = 48;
constexpr int kHeroBadgeCategory = 49;
constexpr int kCultBadgeCategory = 50;
constexpr int kClassHeroBadgeFirst = 928;
constexpr int kClassHeroBadgeLast = 933;
constexpr int kDiceCategoryFirst = 52;
constexpr int kDiceCategoryLast = 56;

}  // namespace

AutosellViewBuildReason autosell_build_view(
    const qol::features::autosell::read_only::InventoryItemRef& ref, autosell::ItemView* out,
    AutosellViewDiagnostics* diagnostics, AttributeQualityApiProvider quality_provider) {
    if (diagnostics != nullptr) *diagnostics = {};
    if (out == nullptr) return AutosellViewBuildReason::kNullOutput;
    *out = autosell::ItemView{};
    if (ref.item.slot == 0 || ref.item.generation == 0)
        return AutosellViewBuildReason::kInvalidHandle;
    if (ref.category <= 0) return AutosellViewBuildReason::kInvalidCategory;
    const int category = ref.category;
    out->category = category;
    if (diagnostics != nullptr) diagnostics->category = category;

    qol::contracts::QolItemInventoryView facts{};
    if (!qol::features::autosell::read_only::item_inventory_view(ref.item, &facts))
        return AutosellViewBuildReason::kFactsUnavailable;
    if (diagnostics != nullptr) {
        diagnostics->fields = facts.fields;
        diagnostics->rarity = facts.rarity;
        diagnostics->equip = facts.equip;
    }
    if ((facts.fields & qol::contracts::kQolItemInventoryHasJewelValue) == 0 ||
        (facts.fields & qol::contracts::kQolItemInventoryHasJewelAttributeType) == 0)
        return AutosellViewBuildReason::kMissingRequiredFields;
    int32_t real_equip = 0;
    if (!qol::features::autosell::read_only::item_is_real_equip(ref.item, &real_equip))
        return AutosellViewBuildReason::kFactsUnavailable;
    out->is_equip = real_equip != 0;
    out->rarity = (facts.fields & qol::contracts::kQolItemInventoryHasRarity) != 0
                      ? facts.rarity
                      : -1;
    out->enhance_remaining = facts.enchant_remaining;
    out->socket_filled = facts.socket_filled;
    out->socket_total = facts.socket_total;

    int32_t value = 0;

    if (!qol::features::autosell::read_only::item_is_jewel(category, &value)) {
        return AutosellViewBuildReason::kJewelPredicateUnavailable;
    }
    out->is_jewel = value != 0;
    if (diagnostics != nullptr) diagnostics->jewel = value;
    if (out->is_jewel) {
        // M-4：IsJewel 命中却 category < 28（闭区间 28..32）属异常，跳过，
        // 避免负 tier 落入宝石阈值命中区间。
        if (category < kJewelCategoryFirst)
            return AutosellViewBuildReason::kInvalidJewelCategory;
        out->jewel_tier = category - kJewelCategoryFirst;

        // 不可用、撤销中或质量查询失败时保留 -1；gemRange 规则不命中，避免默认 0 误售。
        const auto* attr_api = quality_provider == nullptr ? nullptr : quality_provider();
        if (qol::contracts::attribute_quality_api_size_valid(attr_api)) {
            qol::contracts::AttributeQualityOptions options{};
            qol::contracts::AttributeQualityItemResult result{};
            if (attr_api->query_item(ref.item, &options, &result).ok == kQolOk &&
                result.count == 1 && result.complete != 0 &&
                result.attrs[0].kind == qol::contracts::AttributeKind::Jewel &&
                result.attrs[0].status == kQolOk) {
                out->jewel_percentile = result.attrs[0].quality.percentile;
            }
        }
    }

    out->is_backpack = category >= kBackpackCategoryFirst && category <= kBackpackCategoryLast &&
                       qol::features::autosell::read_only::item_is_backpack(category, &value) &&
                       value != 0;
    if (category >= kBadgeRankOneFirst && category <= kBadgeRankOneLast) {
        out->badge_rank = 1;
    } else if (category == kBlessedBadgeCategory) {
        out->badge_rank = 2;
    } else if (category == kCultBadgeCategory) {
        out->badge_rank = 3;
    } else if (category == kHeroBadgeCategory ||
               (category >= kClassHeroBadgeFirst && category <= kClassHeroBadgeLast)) {
        out->badge_rank = 4;
    }
    if (category >= kDiceCategoryFirst && category <= kDiceCategoryLast &&
        qol::features::autosell::read_only::item_is_dice(category, &value) && value != 0) {
        out->dice_tier = category - 51;
    }
    if (diagnostics != nullptr) {
        diagnostics->is_backpack = out->is_backpack;
        diagnostics->badge_rank = out->badge_rank;
        diagnostics->dice_tier = out->dice_tier;
    }
    return AutosellViewBuildReason::kBuilt;
}
