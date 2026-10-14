// autosell_view.cpp —— 数据层物品引用 -> 纯规则视图（阶段 B）。

#include "view.h"

#include "../../contracts/attribute_quality.h"
#include "plugin.h"

#include <cstdint>

namespace {

// 宝石 category 起档（IsJewel 闭区间 28..32；档位 = category - 28）。
constexpr int kJewelCategoryFirst = 28;
// ITEMDATABASE.json 名称核对：category 42..47 为各职业勇士徽章；48 及以上徽章为
// 被祝福/英雄/教团等其他类型，不属于自动出售的普通徽章。
constexpr int kNormalSealFirst = 42;
constexpr int kNormalSealLast = 47;

}  // namespace

AutosellViewBuildReason autosell_build_view(
    const qol::features::autosell::read_only::InventoryItemRef& ref, autosell::ItemView* out,
    AutosellViewDiagnostics* diagnostics) {
    if (diagnostics != nullptr) *diagnostics = {};
    if (out == nullptr) return AutosellViewBuildReason::kNullOutput;
    *out = autosell::ItemView{};
    if (ref.item.slot == 0 || ref.item.generation == 0)
        return AutosellViewBuildReason::kInvalidHandle;
    if (ref.category <= 0) return AutosellViewBuildReason::kInvalidCategory;
    const int category = ref.category;

    qol::contracts::QolItemInventoryView facts{};
    if (!qol::features::autosell::read_only::item_inventory_view(ref.item, &facts))
        return AutosellViewBuildReason::kFactsUnavailable;
    if (diagnostics != nullptr) {
        diagnostics->fields = facts.fields;
        diagnostics->rarity = facts.rarity;
        diagnostics->equip = facts.equip;
    }
    if ((facts.fields & qol::contracts::kQolItemInventoryHasRarity) == 0 ||
        (facts.fields & qol::contracts::kQolItemInventoryHasJewelValue) == 0 ||
        (facts.fields & qol::contracts::kQolItemInventoryHasJewelAttributeType) == 0)
        return AutosellViewBuildReason::kMissingRequiredFields;
    out->is_equip = facts.equip != 0;
    out->rarity = facts.rarity;
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
        const auto* sdk = qol::features::autosell::host_sdk();
        const void* iface = nullptr;
        bool quality_available = false;
        if (sdk != nullptr && sdk->capabilities.require != nullptr) {
            const QolResult require_result = sdk->capabilities.require(
                qol::contracts::kAttributeQualityCapabilityId,
                QolVersionRange{static_cast<uint16_t>(qol::contracts::kAttributeQualityVersionMajor),
                                 static_cast<uint16_t>(qol::contracts::kAttributeQualityVersionMinor)},
                &iface);
            quality_available = require_result.ok == kQolOk;
        }
        const auto* attr_api = static_cast<const qol::contracts::AttributeQualityApi*>(iface);
        if (quality_available && qol::contracts::attribute_quality_api_size_valid(attr_api)) {
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

    uint32_t specials = 0;
    if (qol::features::autosell::read_only::item_is_backpack(category, &value) && value != 0) {
        specials |= autosell::kSpecialBackpack;
    }
    if (category >= kNormalSealFirst && category <= kNormalSealLast) {
        specials |= autosell::kSpecialNormalSeal;
    }
    if (qol::features::autosell::read_only::item_is_dice(category, &value) && value != 0) {
        specials |= autosell::kSpecialDice;
    }
    out->special_types = specials;
    if (diagnostics != nullptr) diagnostics->special_types = specials;
    return AutosellViewBuildReason::kBuilt;
}
