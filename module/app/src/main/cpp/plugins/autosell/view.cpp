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

bool autosell_build_view(
    const qol::features::autosell::read_only::InventoryItemRef& ref, autosell::ItemView* out) {
    if (out == nullptr) return false;
    *out = autosell::ItemView{};
    if (ref.native_item == nullptr) return false;
    if (ref.category <= 0) return false;
    const int category = ref.category;

    // 装备判据用游戏自身的 ITEM_IsRealEquip：不可堆叠（item_is_equip）会把宝石/背包/
    // 徽章等非装备也判为真，导致 rarity/强化/孔数规则过匹配。
    int32_t value = 0;
    if (!qol::features::autosell::read_only::item_is_real_equip(ref.native_item, &value)) {
        return false;
    }
    out->is_equip = value != 0;
    if (!qol::features::autosell::read_only::item_rarity(ref.native_item, &out->rarity)) {
        return false;
    }

    // 强化/孔位位段拆解（auto-sell.md §3.1）：bits2-5=剩余强化次数（筛选口径）、
    // bits6-10=已强化次数、I_SOCKET bits0-3=已镶嵌数（后两者 >0 触发硬保护）、bits4-7=总孔数。
    if (!qol::features::autosell::read_only::item_enchant_remaining(
            ref.native_item, &out->enhance_remaining) ||
        !qol::features::autosell::read_only::item_socket_filled(ref.native_item,
                                                                &out->socket_filled) ||
        !qol::features::autosell::read_only::item_socket_total(ref.native_item,
                                                               &out->socket_total)) {
        return false;
    }

    if (!qol::features::autosell::read_only::item_is_jewel(category, &value)) {
        return false;
    }
    out->is_jewel = value != 0;
    if (out->is_jewel) {
        // M-4：IsJewel 命中却 category < 28（闭区间 28..32）属异常，跳过，
        // 避免负 tier 落入宝石阈值命中区间。
        if (category < kJewelCategoryFirst) return false;
        out->jewel_tier = category - kJewelCategoryFirst;

        // 宝石属性范围：独立宝石自身位域 item+I_COUNT 的 bits0-10=属性值、
        // bits18-23=属性类型。经 attribute.quality 能力探测该属性值的随机区间 [X, 2X]。
        // 过渡期消费方式：每次调用按能力查找（不缓存指针跨帧）；不可用/撤销中/探测失败
        // 时保持 -1（fail-closed，gemRange 规则不命中，不得默认 0 触发误售）。
        int jewel_value = 0;
        int jewel_attr_type = 0;
        if (!qol::features::autosell::read_only::item_jewel_value(ref.native_item, &jewel_value) ||
            !qol::features::autosell::read_only::item_jewel_attribute_type(ref.native_item,
                                                                            &jewel_attr_type)) {
            return false;
        }
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
            if (attr_api->probe_jewel(ref.native_item, jewel_attr_type, jewel_value, &quality).ok ==
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
    return true;
}
