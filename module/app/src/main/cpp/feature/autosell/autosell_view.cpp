// autosell_view.cpp —— 数据层物品引用 -> 纯规则视图（阶段 B）。

#include "feature/autosell/autosell_view.h"

#include "data/native/game_symbols.h"
#include "data/native/item_class.h"
#include "game_access.h"

#include <cstdint>

namespace {

// 宝石 category 起档（IsJewel 闭区间 28..32；档位 = category - 28）。
constexpr int kJewelCategoryFirst = 28;

}  // namespace

bool autosell_build_view(const InventoryItemRef& ref, autosell::ItemView* out) {
    if (out == nullptr) return false;
    *out = autosell::ItemView{};
    if (ref.native_item == nullptr) return false;
    if (ref.category <= 0) return false;
    // M-3 fail-closed：必需符号未解析（或 g_base 未就绪）时跳过该物品，
    // 不得喂默认值（如 rarity=0）触发误售。
    if (fn_get_rarity == nullptr || fn_is_jewel == nullptr) return false;
    if (g_base == 0) return false;

    uint8_t* item = reinterpret_cast<uint8_t*>(ref.native_item);
    const int category = ref.category;

    out->is_equip = item_is_equip(ref.native_item);
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
    }

    uint32_t specials = 0;
    if (item_is_backpack(category)) specials |= autosell::kSpecialBackpack;
    if (fn_is_mercenary_seal != nullptr && fn_is_mercenary_seal(category) != 0) {
        specials |= autosell::kSpecialMercenarySeal;
    }
    if (fn_is_enchant_scroll != nullptr && fn_is_enchant_scroll(category) != 0) {
        specials |= autosell::kSpecialEnchantScroll;
    }
    if (fn_is_dice != nullptr && fn_is_dice(category) != 0) {
        specials |= autosell::kSpecialDice;
    }
    if (fn_is_sealed != nullptr && fn_is_sealed(category) != 0) {
        specials |= autosell::kSpecialSealed;
    }
    if (fn_is_item_box != nullptr && fn_is_item_box(category) != 0) {
        specials |= autosell::kSpecialItemBox;
    }
    out->special_types = specials;
    return true;
}
