// autosell_view.cpp —— 数据层物品引用 -> 纯规则视图（阶段 B）。

#include "feature/autosell/autosell_view.h"

#include "data/native/game_symbols.h"
#include "data/native/item_class.h"
#include "feature/attribute_range/attribute_range.h"
#include "feature/attribute_range/game_ui_attr_range.h"
#include "game_access.h"

#include <cstdint>

namespace {

// 宝石 category 起档（IsJewel 闭区间 28..32；档位 = category - 28）。
constexpr int kJewelCategoryFirst = 28;
// ITEMDATABASE.json 名称核对：category 42..47 为各职业勇士徽章；48 及以上徽章为
// 被祝福/英雄/教团等其他类型，不属于自动出售的普通徽章。
constexpr int kNormalSealFirst = 42;
constexpr int kNormalSealLast = 47;

}  // namespace

bool autosell_build_view(const InventoryItemRef& ref, autosell::ItemView* out) {
    if (out == nullptr) return false;
    *out = autosell::ItemView{};
    if (ref.native_item == nullptr) return false;
    if (ref.category <= 0) return false;
    // M-3 fail-closed：必需符号未解析（或 g_base 未就绪）时跳过该物品，
    // 不得喂默认值（如 rarity=0）触发误售。
    if (fn_get_rarity == nullptr || fn_is_jewel == nullptr || fn_item_is_real_equip == nullptr) {
        return false;
    }
    if (g_base == 0) return false;

    uint8_t* item = reinterpret_cast<uint8_t*>(ref.native_item);
    const int category = ref.category;

    // 装备判据用游戏自身的 ITEM_IsRealEquip：不可堆叠（item_is_equip）会把宝石/背包/
    // 徽章等非装备也判为真，导致 rarity/强化/孔数规则过匹配。
    out->is_equip = fn_item_is_real_equip(ref.native_item) != 0;
    out->rarity = fn_get_rarity(ref.native_item);

    const uint16_t enchant = *reinterpret_cast<uint16_t*>(item + I_ENCHANT);
    out->enhance_count = static_cast<int>((enchant >> 6) & 0x1F);
    const uint8_t socket = *reinterpret_cast<uint8_t*>(item + I_SOCKET);
    out->socket_total = static_cast<int>((socket >> 4) & 0x0F);

    out->is_jewel = fn_is_jewel(category) != 0;
    if (out->is_jewel) {
        // M-4：IsJewel 命中却 category < 28（闭区间 28..32）属异常，跳过，
        // 避免负 tier 落入宝石阈值命中区间。
        if (category < kJewelCategoryFirst) return false;
        out->jewel_tier = category - kJewelCategoryFirst;

        // 宝石属性范围：独立宝石自身位域 item+I_COUNT 的 bits0-10=属性值、
        // bits18-23=属性类型。复用 attribute_range feature 探测该属性值的随机区间
        // [X, 2X]（不复制其内部实现）。探测未安装/失败时保持 -1（fail-closed，
        // gemRange 规则不命中，不得默认 0 触发误售）。
        const uint32_t jewel_bits = *reinterpret_cast<uint32_t*>(item + I_COUNT);
        const int jewel_value = static_cast<int>(jewel_bits & 0x7FF);
        const int jewel_attr_type = static_cast<int>((jewel_bits >> 18) & 0x3F);
        if (attr_range_ready()) {
            int range_min = 0;
            int range_max = 0;
            if (attr_range_probe_jewel_range(jewel_attr_type, ref.native_item, &range_min,
                                             &range_max)) {
                out->jewel_percentile = attr_range::percentile(jewel_value, range_min, range_max);
            }
        }
    }

    uint32_t specials = 0;
    if (item_is_backpack(category)) specials |= autosell::kSpecialBackpack;
    if (category >= kNormalSealFirst && category <= kNormalSealLast) {
        specials |= autosell::kSpecialNormalSeal;
    }
    if (fn_is_dice != nullptr && fn_is_dice(category) != 0) {
        specials |= autosell::kSpecialDice;
    }
    out->special_types = specials;
    return true;
}
