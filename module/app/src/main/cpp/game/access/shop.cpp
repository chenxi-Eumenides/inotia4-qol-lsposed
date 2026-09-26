// shop.cpp —— 商店域读原语（T2l-sho 迁自 api/native/game_shop.cpp，逐字）。

#include "game/access/shop.h"

#include "game/access/game_access.h"
#include "game/symbols/game_symbols.h"

namespace qol::game {

std::string shop_items_json() {
    if (g_base == 0 || fn_item_get_buy_price == nullptr || fn_get_bit == nullptr || fn_get_cumulate_count == nullptr) return "{\"items\":[]}";
    uint8_t* sale_list = reinterpret_cast<uint8_t*>(*(reinterpret_cast<void**>(g_base + G_DEALSYSTEM_SALE_LIST_VMA)));
    if (sale_list == nullptr) return "{\"items\":[]}";
    std::string s = "{\"items\":[";
    bool first = true;
    for (int i = 0; i < 48; ++i) {
        uint8_t* slot = sale_list + i * 16;
        uint64_t flags = *reinterpret_cast<uint64_t*>(slot);
        if (flags & 1) continue;  // bit0=空/已售
        void* item = *reinterpret_cast<void**>(slot + 8);
        if (item == nullptr) continue;
        uint16_t iflags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        uint32_t category = fn_get_bit(iflags, 15, 6);
        uint32_t count = fn_get_cumulate_count(item);
        int price = fn_item_get_buy_price(item);
        if (!first) s += ",";
        first = false;
        s += "{\"slot\":" + std::to_string(i) + ",\"category\":" + std::to_string(category) +
             ",\"count\":" + std::to_string(count) + ",\"price\":" + std::to_string(price) + "}";
    }
    s += "]}";
    return s;
}

}  // namespace qol::game
