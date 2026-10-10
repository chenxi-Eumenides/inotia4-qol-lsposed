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

        // 宝石属性范围：独立宝石自身位域 item+I_COUNT 的 bits0-10=属性值、
        // bits18-23=属性类型。经 attribute.quality 能力探测该属性值的随机区间 [X, 2X]。
        // 过渡期消费方式：每次调用按能力查找（不缓存指针跨帧）；不可用/撤销中/探测失败
        // 时保持 -1（fail-closed，gemRange 规则不命中，不得默认 0 触发误售）。
        const int jewel_value = facts.jewel_value;
        const int jewel_attr_type = facts.jewel_attribute_type;
        const auto* sdk = qol::features::autosell::host_sdk();
        const void* iface = nullptr;
        if (sdk != nullptr && sdk->capabilities.require != nullptr) {
            (void)sdk->capabilities.require(
                qol::contracts::kAttributeQualityCapabilityId,
                QolVersionRange{static_cast<uint16_t>(qol::contracts::kAttributeQualityVersionMajor),
                                static_cast<uint16_t>(qol::contracts::kAttributeQualityVersionMinor)},
                &iface);
        }
        const auto* attr_api = static_cast<const qol::contracts::AttributeQualityApi*>(iface);
        if (attr_api != nullptr && attr_api->probe_jewel != nullptr &&
            attr_api->percentile != nullptr) {
            qol::contracts::AttributeQualityResult quality{};
            if (attr_api->probe_jewel(ref.item, jewel_attr_type, jewel_value, &quality).ok ==
                    kQolOk &&
                attr_api->percentile(jewel_value, quality.min, quality.max,
                                     &out->jewel_percentile).ok == kQolOk) {
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
